#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace vision {

using LatencyClock = std::chrono::steady_clock;

enum class LatencyMetric : std::uint8_t {
    ai_rga_age,
    ai_queue,
    ai_infer,
    ai_total,
    osd_wait,
    osd_update,
    main_venc_age,
    main_rtsp_wait,
    main_send,
    main_server_total,
    sub_venc_age,
    sub_rtsp_wait,
    sub_send,
    sub_server_total,
};

inline const char* latency_metric_name(LatencyMetric metric) {
    switch (metric) {
    case LatencyMetric::ai_rga_age: return "ai.rga_age";
    case LatencyMetric::ai_queue: return "ai.queue";
    case LatencyMetric::ai_infer: return "ai.infer";
    case LatencyMetric::ai_total: return "ai.total";
    case LatencyMetric::osd_wait: return "osd.wait";
    case LatencyMetric::osd_update: return "osd.update";
    case LatencyMetric::main_venc_age: return "main.venc_age";
    case LatencyMetric::main_rtsp_wait: return "main.rtsp_wait";
    case LatencyMetric::main_send: return "main.send";
    case LatencyMetric::main_server_total: return "main.server_total";
    case LatencyMetric::sub_venc_age: return "sub.venc_age";
    case LatencyMetric::sub_rtsp_wait: return "sub.rtsp_wait";
    case LatencyMetric::sub_send: return "sub.send";
    case LatencyMetric::sub_server_total: return "sub.server_total";
    }
    return "unknown";
}

struct LatencyRecord {
    std::uint64_t observed_us = 0;
    std::uint64_t frame_pts_us = 0;
    std::uint64_t latency_us = 0;
    LatencyMetric metric = LatencyMetric::ai_rga_age;
};

// Producers only append to a bounded in-memory queue. A dedicated thread owns
// the file, batches records and flushes once per second, so slow storage can
// drop diagnostics but can never backpressure the real-time media graph.
class AsyncLatencyCsv {
public:
    explicit AsyncLatencyCsv(std::string path, std::size_t capacity = 8192)
        : path_(std::move(path)), capacity_(capacity), output_(path_) {
        if (!capacity_) throw std::invalid_argument("latency CSV queue capacity must be positive");
        if (!output_)
            throw std::runtime_error("cannot open latency CSV: " + path_);
        output_ << "observed_monotonic_us,frame_pts_us,stage,latency_us,latency_ms\n";
        output_.flush();
        if (!output_)
            throw std::runtime_error("cannot write latency CSV header: " + path_);
        writer_ = std::thread([this] { writer_loop(); });
    }

    ~AsyncLatencyCsv() { close(); }
    AsyncLatencyCsv(const AsyncLatencyCsv&) = delete;
    AsyncLatencyCsv& operator=(const AsyncLatencyCsv&) = delete;

    void push(LatencyMetric metric, LatencyClock::time_point observed_at,
              std::uint64_t frame_pts_us, std::uint64_t latency_us) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_ || failed_.load() || records_.size() == capacity_) {
                ++dropped_;
                return;
            }
            records_.push_back({clock_us(observed_at), frame_pts_us, latency_us, metric});
            high_water_ = std::max(high_water_, records_.size());
        }
        ready_.notify_one();
    }

    void close() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) return;
            closed_ = true;
        }
        ready_.notify_all();
        if (writer_.joinable()) writer_.join();
    }

    std::uint64_t written() const { return written_.load(); }
    std::uint64_t dropped() const { return dropped_.load(); }
    bool failed() const { return failed_.load(); }
    std::size_t capacity() const { return capacity_; }
    std::size_t queued() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return records_.size();
    }
    std::size_t high_water() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return high_water_;
    }
    const std::string& path() const { return path_; }

private:
    static std::uint64_t clock_us(LatencyClock::time_point value) {
        const auto count = std::chrono::duration_cast<std::chrono::microseconds>(
                               value.time_since_epoch()).count();
        return count < 0 ? 0 : static_cast<std::uint64_t>(count);
    }

    void writer_loop() noexcept {
        auto last_flush = LatencyClock::now();
        for (;;) {
            std::deque<LatencyRecord> batch;
            bool finished = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait_for(lock, std::chrono::milliseconds(250),
                                [this] { return closed_ || !records_.empty(); });
                batch.swap(records_);
                finished = closed_;
            }

            for (const auto& record : batch) {
                output_ << record.observed_us << ',' << record.frame_pts_us << ','
                        << latency_metric_name(record.metric) << ',' << record.latency_us << ','
                        << std::fixed << std::setprecision(3)
                        << record.latency_us / 1000.0 << '\n';
            }
            written_.fetch_add(static_cast<std::uint64_t>(batch.size()));
            if (!output_) {
                failed_.store(true);
                return;
            }
            const auto now = LatencyClock::now();
            if (finished || now - last_flush >= std::chrono::seconds(1)) {
                output_.flush();
                last_flush = now;
                if (!output_) {
                    failed_.store(true);
                    return;
                }
            }
            if (finished) return;
        }
    }

    const std::string path_;
    const std::size_t capacity_;
    std::ofstream output_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<LatencyRecord> records_;
    std::thread writer_;
    std::atomic<std::uint64_t> written_{0}, dropped_{0};
    std::atomic<bool> failed_{false};
    std::size_t high_water_ = 0;
    bool closed_ = false;
};

} // namespace vision
