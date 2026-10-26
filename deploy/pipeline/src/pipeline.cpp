#include "vision/pipeline.hpp"

#include "vision/audio.hpp"
#include "vision/core.hpp"
#include "vision/latency_trace.hpp"
#include "vision/rk_backend.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace vision {
namespace {
using Clock = std::chrono::steady_clock;

uint64_t elapsed_ms(Clock::time_point start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
}

uint64_t duration_us(Clock::duration duration) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(duration).count());
}

uint64_t monotonic_us(Clock::time_point value) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            value.time_since_epoch()).count());
}

bool pts_age_us(uint64_t pts_us, Clock::time_point now, uint64_t& age_us) {
    if (!pts_us) return false;
    const auto signed_now =
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    if (signed_now < 0 || static_cast<uint64_t>(signed_now) < pts_us) return false;
    age_us = static_cast<uint64_t>(signed_now) - pts_us;
    // Reject a different timestamp epoch and stale buffers instead of printing
    // plausible-looking but meaningless multi-minute latency.
    return age_us <= 60000000u;
}

std::string format_latency(const char* name, const LatencySnapshot& value) {
    std::ostringstream out;
    out << name << '=';
    if (!value.window) return out.str() + "-";
    out << std::fixed << std::setprecision(2)
        << value.p50_us / 1000.0 << '/'
        << value.p95_us / 1000.0 << '/'
        << value.p99_us / 1000.0 << '/'
        << value.max_us / 1000.0
        << "(n=" << value.window << ",total=" << value.samples << ')';
    return out.str();
}

struct AiFrame {
    Frame frame;
    Clock::time_point queued_at{};
};

struct EncodedPacket {
    int stream = 0;
    Frame frame;
    Clock::time_point ready_at{};
};

struct AudioPacket {
    std::vector<std::uint8_t> data;
    std::uint64_t pts_us = 0;
    Clock::time_point ready_at{};
};
} // namespace

class Pipeline::Impl {
public:
    explicit Impl(Config config)
        : config_(std::move(config)), ai_frames_(1),
          encoded_packets_(static_cast<std::size_t>(config_.packet_queue)),
          audio_packets_(16) {
        if (!config_.latency_csv.empty()) config_.latency_stats = true;
        config_.validate();
    }

    ~Impl() { stop(); }

    void start() {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        if (started_) throw std::logic_error("Pipeline::start may only be called once");
        started_ = true;
        try {
            if (!config_.latency_csv.empty())
                latency_csv_.reset(new AsyncLatencyCsv(config_.latency_csv));
            rtsp_.reset(new RtspServer(config_.rtsp_port, config_.audio));
            isp_.reset(new Isp(config_));
            media_.reset(new MediaSystem());
            const bool offline_ispp = config_.sensor_mode == "4k-ispp-1080";
            const std::string& main_node = offline_ispp
                                               ? isp_->offline_main_node()
                                               : config_.main_node;
            const std::string& sub_node = offline_ispp
                                              ? isp_->offline_sub_node()
                                              : config_.sub_node;
            main_vi_.reset(new ViChannel(config_.camera, 0, main_node,
                                         config_.width, config_.height,
                                         config_.pool_buffers));
            sub_vi_.reset(new ViChannel(config_.camera, 1, sub_node,
                                        config_.sub_width, config_.sub_height,
                                        config_.pool_buffers));
            if (offline_ispp) {
                // ISP mainpath owns the hardware 4K -> 1080p full-FOV resize.
                // RKMedia passes its exported DMA-BUF directly to the ISPP
                // virtual input; no CPU mapping or pixel copy is introduced.
                isp_main_vi_.reset(new ViChannel(config_.camera, 2,
                                                  "rkisp_mainpath", 1920, 1080,
                                                  config_.pool_buffers));
                ispp_input_.reset(new VpChannel(0, 0,
                                                 isp_->offline_input_node(),
                                                 1920, 1080,
                                                 config_.pool_buffers));
            }

            // Main path stays NV12 in DMA-BUF from VI through RGA into VENC.
            // RGA performs rotation and persistent hardware region composition.
            main_rga_.reset(new RgaChannel(0, config_.width, config_.height,
                                           IMAGE_TYPE_NV12, config_.rotation,
                                           config_.pool_buffers));
            // Sub video has its own NV12 hardware path. RockX gets a separate
            // low-resolution BGR RGA branch, so AI never owns a sub-video frame.
            sub_rga_.reset(new RgaChannel(1, config_.sub_width, config_.sub_height,
                                          IMAGE_TYPE_NV12, config_.rotation,
                                          config_.pool_buffers));
            if (config_.ai) {
                ai_rga_.reset(new RgaChannel(2, config_.sub_width, config_.sub_height,
                                             IMAGE_TYPE_BGR888, config_.rotation,
                                             config_.pool_buffers));
                // Apply the rate limit at the RGA input. RKMedia drops frames
                // in Flow::SendInput(), before allocating/converting BGR, so
                // a 3 FPS detector no longer converts all 30 video FPS.
                ai_rga_->limit_input_rate(config_.fps, config_.ai_fps);
            }
            main_venc_.reset(new VencChannel(0, config_.output_width(),
                                              config_.output_height(),
                                              config_.output_width(),
                                              config_.output_height(),
                                              config_.fps, config_.bitrate));
            sub_venc_.reset(new VencChannel(1, config_.sub_output_width(),
                                             config_.sub_output_height(),
                                             config_.sub_output_width(),
                                             config_.sub_output_height(),
                                             config_.fps, config_.sub_bitrate));
            main_venc_bind_.reset(new Binding(main_rga_->endpoint(),
                                               MPP_CHN_S{RK_ID_VENC, 0, 0}));
            sub_venc_bind_.reset(new Binding(sub_rga_->endpoint(),
                                              MPP_CHN_S{RK_ID_VENC, 0, 1}));
            compositor_.reset(new HardwareCompositor(0, config_));
            if (config_.ai) detector_.reset(new ObjectDetector(config_));
            // Connect the hardware graph downstream first, then release VI
            // frames into it. This avoids filling RGA output buffers during
            // construction before a consumer exists.
            main_bind_.reset(new Binding(main_vi_->endpoint(), main_rga_->endpoint()));
            sub_bind_.reset(new Binding(sub_vi_->endpoint(), sub_rga_->endpoint()));
            if (config_.ai)
                ai_bind_.reset(new Binding(sub_vi_->endpoint(), ai_rga_->endpoint()));
            // Start the offline source last. ISPP output channels must already
            // have active consumers before VP queues the first input DMA-BUF;
            // this is also the ordering required by Rockchip's VI-VP demo.
            if (ispp_input_)
                isp_to_ispp_.reset(new Binding(isp_main_vi_->endpoint(),
                                                ispp_input_->endpoint()));

            // Open/start ALSA only after the expensive ISP and RKNN setup so
            // the capture ring cannot overrun before its consumer exists.
            if (config_.audio) audio_capture_.reset(new AlsaCapture(config_));

            running_.store(true);
            // VI channels can retain frames while the rest of the hardware
            // graph and RKNN model are being constructed. Do not let those
            // cold-start buffers pollute steady-state latency percentiles.
            latency_start_ = Clock::now() + std::chrono::seconds(3);
            if (config_.ai) {
                launch("ai-capture", [this] { ai_capture_loop(); });
                launch("ai", [this] { ai_loop(); });
            }
            if (config_.ai && config_.osd) launch("rga-overlay", [this] { overlay_loop(); });
            launch("main-venc", [this] { venc_loop(0); });
            launch("sub-venc", [this] { venc_loop(1); });
            if (config_.audio) launch("alsa-audio", [this] { audio_loop(); });
            launch("rtsp", [this] { rtsp_loop(); });
        } catch (...) {
            running_.store(false);
            close_queues();
            join_threads();
            if (latency_csv_) latency_csv_->close();
            teardown();
            throw;
        }
    }

    void stop() noexcept {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        if (!started_ || stopped_) return;
        stopped_ = true;
        running_.store(false);
        close_queues();
        sleep_cv_.notify_all();
        join_threads();
        if (latency_csv_) latency_csv_->close();
        teardown();
    }

    bool failed() const { return failed_.load(); }
    std::string error() const {
        std::lock_guard<std::mutex> lock(error_mutex_);
        return error_;
    }
    std::string statistics() const {
        std::ostringstream out;
        out << "video main(venc=" << venc_frames_[0].load()
            << ", pts_gap=" << pts_gaps_[0].load()
            << ", pts_reset=" << pts_resets_[0].load()
            << ") sub(venc=" << venc_frames_[1].load()
            << ", pts_gap=" << pts_gaps_[1].load()
            << ", pts_reset=" << pts_resets_[1].load()
            << ") ai(source=" << ai_source_.load()
            << ", sampled=" << ai_sampled_.load()
            << ", completed=" << ai_completed_.load()
            << ", stale_drop=" << ai_frames_.dropped()
            << ") osd(boxes=" << overlay_boxes_.load()
            << ", truncated=" << overlay_truncated_.load()
            << ") rtsp(sent=" << packets_sent_.load()
            << ", queue_high_water=" << encoded_packets_.high_water()
            << '/' << config_.packet_queue
            << ", shutdown_abandoned=" << encoded_packets_.abandoned() << ')';
        if (config_.audio) {
            out << " audio(captured=" << audio_captured_.load()
                << ", sent=" << audio_sent_.load()
                << ", queue_drop=" << audio_packets_.dropped()
                << ", recoveries="
                << (audio_capture_ ? audio_capture_->recoveries() : audio_recoveries_.load())
                << ')';
        } else {
            out << " audio=off";
        }
        if (config_.latency_stats) {
            out << "\nlatency_ms p50/p95/p99/max "
                << "ai(" << format_latency("rga_age", ai_rga_age_.snapshot()) << ' '
                << format_latency("queue", ai_queue_.snapshot()) << ' '
                << format_latency("infer", ai_infer_.snapshot()) << ' '
                << format_latency("total", ai_total_.snapshot()) << ") "
                << "osd(" << format_latency("wait", osd_wait_.snapshot()) << ' '
                << format_latency("update", osd_update_.snapshot()) << ") "
                << "main(" << format_latency("venc_age", venc_age_[0].snapshot()) << ' '
                << format_latency("rtsp_wait", rtsp_wait_[0].snapshot()) << ' '
                << format_latency("send", rtsp_send_[0].snapshot()) << ' '
                << format_latency("server_total", rtsp_total_[0].snapshot()) << ") "
                << "sub(" << format_latency("venc_age", venc_age_[1].snapshot()) << ' '
                << format_latency("rtsp_wait", rtsp_wait_[1].snapshot()) << ' '
                << format_latency("send", rtsp_send_[1].snapshot()) << ' '
                << format_latency("server_total", rtsp_total_[1].snapshot()) << ")"
                << " invalid_pts=" << invalid_latency_pts_.load();
            if (latency_csv_) {
                out << " csv(written=" << latency_csv_->written()
                    << ", dropped=" << latency_csv_->dropped()
                    << ", queued=" << latency_csv_->queued()
                    << ", high_water=" << latency_csv_->high_water()
                    << '/' << latency_csv_->capacity()
                    << ", error=" << (latency_csv_->failed() ? 1 : 0)
                    << ", path=" << latency_csv_->path() << ')';
            }
        }
        return out.str();
    }

private:
    void launch(const char* name, std::function<void()> work) {
        threads_.emplace_back([this, name, work] {
            try { work(); }
            catch (const std::exception& error) { fail(name, error.what()); }
            catch (...) { fail(name, "unknown exception"); }
        });
    }

    void fail(const char* component, const std::string& message) noexcept {
        {
            std::lock_guard<std::mutex> lock(error_mutex_);
            if (!failed_.exchange(true)) error_ = std::string(component) + ": " + message;
        }
        running_.store(false);
        close_queues();
        sleep_cv_.notify_all();
    }

    void close_queues() noexcept {
        ai_frames_.close();
        encoded_packets_.close();
        audio_packets_.close();
        detections_.close();
    }

    void join_threads() noexcept {
        for (auto& thread : threads_)
            if (thread.joinable()) thread.join();
        threads_.clear();
    }

    void teardown() noexcept {
        detector_.reset();
        // Stop new frames before releasing ARGB label storage referenced by
        // RGA regions. UnBind is the synchronization boundary for the graph.
        ai_bind_.reset();
        sub_bind_.reset();
        main_bind_.reset();
        // Stop feeding the offline ISPP input before destroying either end.
        isp_to_ispp_.reset();
        compositor_.reset();
        if (audio_capture_) audio_recoveries_.store(audio_capture_->recoveries());
        audio_capture_.reset();
        rtsp_.reset();
        sub_venc_bind_.reset();
        main_venc_bind_.reset();
        sub_venc_.reset();
        main_venc_.reset();
        ai_rga_.reset();
        sub_rga_.reset();
        main_rga_.reset();
        sub_vi_.reset();
        main_vi_.reset();
        ispp_input_.reset();
        isp_main_vi_.reset();
        media_.reset();
        isp_.reset();
    }

    bool interruptible_delay(int milliseconds) {
        if (!milliseconds) return running_.load();
        std::unique_lock<std::mutex> lock(sleep_mutex_);
        sleep_cv_.wait_for(lock, std::chrono::milliseconds(milliseconds),
                           [this] { return !running_.load(); });
        return running_.load();
    }

    bool latency_active(Clock::time_point now) const {
        return config_.latency_stats && now >= latency_start_;
    }

    void observe_latency(LatencyMetric metric, LatencyWindow& window,
                         uint64_t pts, Clock::time_point now, uint64_t latency_us) {
        if (!latency_active(now)) return;
        window.observe_us(latency_us);
        if (latency_csv_) latency_csv_->push(metric, now, pts, latency_us);
    }

    void observe_pts_age(LatencyMetric metric, LatencyWindow& window, uint64_t pts,
                         Clock::time_point now) {
        if (!latency_active(now)) return;
        uint64_t age = 0;
        if (pts_age_us(pts, now, age)) observe_latency(metric, window, pts, now, age);
        else ++invalid_latency_pts_;
    }

    void ai_capture_loop() {
        auto last_frame = Clock::now();
        while (running_.load()) {
            auto source = get_buffer(RK_ID_RGA, 2, 100);
            if (!source) {
                if (elapsed_ms(last_frame) > static_cast<uint64_t>(config_.stall_timeout_ms))
                    throw std::runtime_error("AI source stalled");
                continue;
            }
            const auto ready_at = Clock::now();
            last_frame = ready_at;
            observe_pts_age(LatencyMetric::ai_rga_age, ai_rga_age_, source->pts(), ready_at);
            ++ai_source_;
            ++ai_sampled_;
            AiFrame sampled;
            sampled.frame = std::move(source);
            sampled.queued_at = Clock::now();
            if (!ai_frames_.push(std::move(sampled))) return;
        }
    }

    void ai_loop() {
        AiFrame source;
        while (running_.load()) {
            if (!ai_frames_.pop(source, std::chrono::milliseconds(100))) continue;
            const auto dequeued_at = Clock::now();
            if (latency_active(dequeued_at) && source.queued_at != Clock::time_point{})
                observe_latency(LatencyMetric::ai_queue, ai_queue_, source.frame->pts(),
                                dequeued_at, duration_us(dequeued_at - source.queued_at));
            if (!interruptible_delay(config_.ai_delay_ms)) return;
            const auto infer_start = Clock::now();
            auto detection = detector_->infer(source.frame);
            const auto infer_end = Clock::now();
            detection.completed_at = infer_end;
            if (latency_active(infer_end))
                observe_latency(LatencyMetric::ai_infer, ai_infer_, detection.pts_us,
                                infer_end, duration_us(infer_end - infer_start));
            observe_pts_age(LatencyMetric::ai_total, ai_total_, detection.pts_us, infer_end);
            detections_.publish(std::move(detection));
            ++ai_completed_;
        }
    }

    void overlay_loop() {
        uint64_t applied_pts = 0;
        uint64_t detection_generation = 0;
        bool applied = false;
        bool visible = false;
        const auto result_ttl = std::chrono::milliseconds(config_.result_ttl_ms);
        while (running_.load()) {
            const auto result = detections_.latest(result_ttl);
            if (result && (!applied || result->pts_us != applied_pts)) {
                auto boxes = scale_boxes(*result, config_.output_width(),
                                         config_.output_height());
                const auto update_start = Clock::now();
                if (latency_active(update_start) && update_start >= result->completed_at)
                    observe_latency(LatencyMetric::osd_wait, osd_wait_, result->pts_us,
                                    update_start,
                                    duration_us(update_start - result->completed_at));
                const std::size_t displayed = compositor_->update(boxes);
                const auto update_end = Clock::now();
                if (latency_active(update_end))
                    observe_latency(LatencyMetric::osd_update, osd_update_, result->pts_us,
                                    update_end, duration_us(update_end - update_start));
                overlay_boxes_.store(displayed);
                if (boxes.size() > displayed)
                    overlay_truncated_.fetch_add(boxes.size() - displayed);
                applied_pts = result->pts_us;
                applied = true;
                visible = displayed != 0;
            } else if (!result && visible) {
                compositor_->update({});
                overlay_boxes_.store(0);
                applied = false;
                visible = false;
            } else if (!result) {
                applied = false;
            }

            // AI publishes at only a few FPS. Sleep until the next result
            // instead of polling 50 times/s, but still wake exactly when the
            // current result reaches its TTL so stale boxes are cleared.
            auto wait = std::chrono::milliseconds(1000);
            if (result) {
                const auto expires = result->completed_at + result_ttl;
                const auto now = Clock::now();
                if (expires <= now) {
                    wait = std::chrono::milliseconds(0);
                } else {
                    const auto remaining_us = duration_us(expires - now);
                    wait = std::chrono::milliseconds((remaining_us + 999) / 1000);
                }
            }
            if (!detections_.wait_for_change(detection_generation, wait) &&
                !running_.load()) {
                return;
            }
        }
    }

    bool account_pts(int stream, uint64_t pts) {
        if (!pts) return false;
        const uint64_t previous = last_pts_[stream].exchange(pts);
        if (pts == previous) return false;
        if (!previous || pts < previous) return true;
        const uint64_t period = 1000000u / static_cast<uint64_t>(config_.fps);
        const uint64_t delta = pts - previous;
        // Camera/ISP startup can advance the monotonic timestamp by several
        // seconds before steady capture begins. Record that as a clock reset,
        // not hundreds of fictitious dropped frames.
        if (delta > period * static_cast<uint64_t>(config_.fps)) {
            ++pts_resets_[stream];
            return true;
        }
        if (delta > period + period / 2) {
            const uint64_t periods = (delta + period / 2) / period;
            if (periods > 1) pts_gaps_[stream].fetch_add(periods - 1);
        }
        return true;
    }

    void venc_loop(int stream) {
        auto last_packet = Clock::now();
        while (running_.load()) {
            auto packet = get_buffer(RK_ID_VENC, stream, 100);
            if (!packet) {
                if (elapsed_ms(last_packet) > static_cast<uint64_t>(config_.stall_timeout_ms))
                    throw std::runtime_error("encoder output stalled");
                continue;
            }
            const auto ready_at = Clock::now();
            last_packet = ready_at;
            observe_pts_age(stream ? LatencyMetric::sub_venc_age
                                   : LatencyMetric::main_venc_age,
                            venc_age_[stream], packet->pts(), ready_at);
            if (account_pts(stream, packet->pts())) ++venc_frames_[stream];
            EncodedPacket encoded;
            encoded.stream = stream;
            encoded.frame = std::move(packet);
            encoded.ready_at = ready_at;
            if (!encoded_packets_.push(std::move(encoded))) return;
        }
    }

    void audio_loop() {
        const std::size_t packet_frames = audio_capture_->frames_per_packet();
        const unsigned channels = audio_capture_->channels();
        const std::uint64_t packet_duration_us =
            packet_frames * 1000000u / AlsaCapture::sample_rate;
        std::vector<std::int16_t> pcm(packet_frames * channels);
        std::size_t collected = 0;
        std::uint64_t next_pts_us = 0;
        std::uint64_t recoveries = audio_capture_->recoveries();
        auto last_samples = Clock::now();

        while (running_.load()) {
            const std::size_t received = audio_capture_->read_frames(
                pcm.data() + collected * channels,
                packet_frames - collected, 100);
            const std::uint64_t current_recoveries = audio_capture_->recoveries();
            if (current_recoveries != recoveries) {
                recoveries = current_recoveries;
                collected = 0;
                next_pts_us = 0;
                last_samples = Clock::now();
                continue;
            }
            if (!received) {
                if (elapsed_ms(last_samples) >
                    static_cast<std::uint64_t>(config_.stall_timeout_ms))
                    throw std::runtime_error("ALSA capture stalled");
                continue;
            }
            last_samples = Clock::now();
            collected += received;
            if (collected < packet_frames) continue;

            const auto ready_at = Clock::now();
            const std::uint64_t observed_pts = monotonic_us(ready_at) - packet_duration_us;
            if (!next_pts_us || observed_pts > next_pts_us + 100000u ||
                next_pts_us > observed_pts + 100000u)
                next_pts_us = observed_pts;

            AudioPacket packet;
            encode_g711a_mono(pcm.data(), packet_frames, channels, packet.data);
            packet.pts_us = next_pts_us;
            packet.ready_at = ready_at;
            next_pts_us += packet_duration_us;
            collected = 0;
            ++audio_captured_;
            if (!audio_packets_.push(std::move(packet))) return;
        }
    }

    void rtsp_loop() {
        EncodedPacket packet;
        AudioPacket audio;
        while (running_.load()) {
            if (encoded_packets_.pop(packet, std::chrono::milliseconds(10))) {
                const auto send_start = Clock::now();
                if (latency_active(send_start) && packet.ready_at != Clock::time_point{})
                    observe_latency(packet.stream ? LatencyMetric::sub_rtsp_wait
                                                  : LatencyMetric::main_rtsp_wait,
                                    rtsp_wait_[packet.stream], packet.frame->pts(), send_start,
                                    duration_us(send_start - packet.ready_at));
                rtsp_->send(packet.stream, packet.frame);
                const auto send_end = Clock::now();
                if (latency_active(send_end))
                    observe_latency(packet.stream ? LatencyMetric::sub_send
                                                  : LatencyMetric::main_send,
                                    rtsp_send_[packet.stream], packet.frame->pts(), send_end,
                                    duration_us(send_end - send_start));
                observe_pts_age(packet.stream ? LatencyMetric::sub_server_total
                                              : LatencyMetric::main_server_total,
                                rtsp_total_[packet.stream], packet.frame->pts(), send_end);
                ++packets_sent_;
                packet.frame.reset();
            }
            if (audio_packets_.pop(audio, std::chrono::milliseconds(0))) {
                rtsp_->send_audio(audio.data, audio.pts_us);
                ++audio_sent_;
                audio.data.clear();
            }
            rtsp_->poll();
        }
    }

    Config config_;
    mutable std::mutex state_mutex_, error_mutex_, sleep_mutex_;
    std::condition_variable sleep_cv_;
    bool started_ = false, stopped_ = false;
    std::atomic<bool> running_{false}, failed_{false};
    std::string error_;
    LatestQueue<AiFrame> ai_frames_;
    BlockingQueue<EncodedPacket> encoded_packets_;
    LatestQueue<AudioPacket> audio_packets_;
    DetectionCache detections_{8};
    std::vector<std::thread> threads_;

    std::unique_ptr<MediaSystem> media_;
    std::unique_ptr<Isp> isp_;
    std::unique_ptr<ViChannel> main_vi_, sub_vi_;
    std::unique_ptr<ViChannel> isp_main_vi_;
    std::unique_ptr<VpChannel> ispp_input_;
    std::unique_ptr<RgaChannel> main_rga_, sub_rga_, ai_rga_;
    std::unique_ptr<Binding> main_bind_, sub_bind_, ai_bind_;
    std::unique_ptr<Binding> isp_to_ispp_;
    std::unique_ptr<Binding> main_venc_bind_, sub_venc_bind_;
    std::unique_ptr<VencChannel> main_venc_, sub_venc_;
    std::unique_ptr<HardwareCompositor> compositor_;
    std::unique_ptr<ObjectDetector> detector_;
    std::unique_ptr<AlsaCapture> audio_capture_;
    std::unique_ptr<RtspServer> rtsp_;
    std::unique_ptr<AsyncLatencyCsv> latency_csv_;

    std::atomic<uint64_t> ai_source_{0}, ai_sampled_{0}, ai_completed_{0};
    std::atomic<uint64_t> packets_sent_{0};
    std::atomic<uint64_t> audio_captured_{0}, audio_sent_{0}, audio_recoveries_{0};
    std::atomic<uint64_t> venc_frames_[2]{{0}, {0}};
    std::atomic<uint64_t> last_pts_[2]{{0}, {0}};
    std::atomic<uint64_t> pts_gaps_[2]{{0}, {0}};
    std::atomic<uint64_t> pts_resets_[2]{{0}, {0}};
    std::atomic<uint64_t> overlay_boxes_{0}, overlay_truncated_{0};
    Clock::time_point latency_start_{};
    LatencyWindow ai_rga_age_, ai_queue_, ai_infer_, ai_total_;
    LatencyWindow osd_wait_, osd_update_;
    LatencyWindow venc_age_[2], rtsp_wait_[2], rtsp_send_[2], rtsp_total_[2];
    std::atomic<uint64_t> invalid_latency_pts_{0};
};

Pipeline::Pipeline(Config config) : impl_(new Impl(std::move(config))) {}
Pipeline::~Pipeline() = default;
void Pipeline::start() { impl_->start(); }
void Pipeline::stop() noexcept { impl_->stop(); }
bool Pipeline::failed() const { return impl_->failed(); }
std::string Pipeline::error() const { return impl_->error(); }
std::string Pipeline::statistics() const { return impl_->statistics(); }
} // namespace vision

