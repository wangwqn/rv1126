#pragma once

#include "vision/config.hpp"
#include "vision/core.hpp"
#include "rkmedia_api.h"
#include "objects.hpp"
#include "librtsp/rtsp_demo.h"
#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace vision {
void check_rk(int status, const char* operation);
class OfflineIsppTopology;

class Noncopyable {
protected:
    Noncopyable() = default;
    ~Noncopyable() = default;
public:
    Noncopyable(const Noncopyable&) = delete;
    Noncopyable& operator=(const Noncopyable&) = delete;
};

// Own the SDK handle, not just its borrowed DMA fd or mapping.
class MediaBuffer : Noncopyable {
public:
    explicit MediaBuffer(MEDIA_BUFFER buffer);
    ~MediaBuffer();
    MEDIA_BUFFER get() const { return buffer_; }
    int fd() const;
    uint64_t pts() const;
    MB_IMAGE_INFO_S image_info() const;
private:
    MEDIA_BUFFER buffer_;
};
using Frame = std::shared_ptr<MediaBuffer>;
Frame get_buffer(MOD_ID_E module, int channel, int timeout_ms);

class MediaSystem : Noncopyable {
public:
    MediaSystem();
};

class Isp : Noncopyable {
public:
    explicit Isp(const Config& c);
    ~Isp();
    void start();
    const std::string& offline_main_node() const;
    const std::string& offline_sub_node() const;
    const std::string& offline_input_node() const;
private:
    int camera_;
    void* context_ = nullptr;
    bool started_ = false;
    int fps_ = 30;
    std::unique_ptr<OfflineIsppTopology> offline_topology_;
};

class ViChannel : Noncopyable {
public:
    ViChannel(int camera, int channel, std::string node, int width, int height, int buffers);
    ~ViChannel();
    MPP_CHN_S endpoint() const;
private:
    int camera_, channel_;
    std::string node_;
};

class VpChannel : Noncopyable {
public:
    VpChannel(int pipe, int channel, std::string node, int width, int height,
              int buffers);
    ~VpChannel();
    MPP_CHN_S endpoint() const;
private:
    int pipe_, channel_;
    std::string node_;
};

class RgaChannel : Noncopyable {
public:
    RgaChannel(int channel, int width, int height, IMAGE_TYPE_E output, int rotation, int buffers);
    ~RgaChannel();
    MPP_CHN_S endpoint() const;
    // Drop excess input frames before the RGA transform. This is used by the
    // AI branch so a low inference rate does not waste bandwidth converting
    // every video frame to BGR.
    void limit_input_rate(int input_fps, int output_fps);
private:
    int channel_;
};

class Binding : Noncopyable {
public:
    Binding(MPP_CHN_S source, MPP_CHN_S destination);
    ~Binding();
private:
    MPP_CHN_S source_, destination_;
};

class VencChannel : Noncopyable {
public:
    VencChannel(int channel, int width, int height, int stride, int vstride, int fps, int bitrate);
    ~VencChannel();
    void send(const Frame& frame);
private:
    int channel_;
};

class HardwareCompositor : Noncopyable {
public:
    HardwareCompositor(int rga_channel, const Config& config);
    ~HardwareCompositor();
    std::size_t update(const std::vector<Box>& boxes);
private:
    void disable_slot(int slot) noexcept;
    void disable_all() noexcept;
    int channel_, height_;
    bool enabled_;
    std::unique_ptr<object_detection::LabelPainter> painter_;
    std::array<std::shared_ptr<cv::Mat>, 2> current_labels_;
    std::array<std::shared_ptr<cv::Mat>, 8> retired_labels_;
    std::size_t retired_index_ = 0;
};

class ObjectDetector : Noncopyable {
public:
    explicit ObjectDetector(const Config& config);
    ~ObjectDetector();
    Detection infer(const Frame& frame);
private:
    object_detection::Model model_;
};

class AlsaCapture : Noncopyable {
public:
    static constexpr unsigned sample_rate = 8000;
    explicit AlsaCapture(const Config& config);
    ~AlsaCapture();
    std::size_t read_frames(std::int16_t* interleaved, std::size_t frames,
                            int timeout_ms);
    unsigned channels() const { return channels_; }
    std::size_t frames_per_packet() const { return frames_per_packet_; }
    std::uint64_t recoveries() const { return recoveries_.load(); }
private:
    void* pcm_ = nullptr;
    unsigned channels_ = 0;
    std::size_t frames_per_packet_ = 0;
    std::atomic<std::uint64_t> recoveries_{0};
};

// This object is serviced exclusively by one RTSP worker.
class RtspServer : Noncopyable {
public:
    RtspServer(int port, bool audio);
    ~RtspServer();
    void send(int stream, const Frame& packet);
    void send_audio(const std::vector<std::uint8_t>& packet, std::uint64_t pts_us);
    void poll();
private:
    rtsp_demo_handle server_ = nullptr;
    rtsp_session_handle sessions_[2] = {nullptr, nullptr};
};
} // namespace vision
