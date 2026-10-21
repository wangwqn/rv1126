#pragma once

#include <cstdint>
#include <cmath>
#include <stdexcept>
#include <string>
#include <ostream>

namespace vision {

struct Config {
    int camera = 0;
    std::string main_node = "rkispp_scale0";
    std::string sub_node = "rkispp_scale1";
    int width = 1920, height = 1080;
    int sub_width = 640, sub_height = 360;
    int rotation = 0;
    int fps = 30, ai_fps = 5;
    int bitrate = 8000000, sub_bitrate = 800000;
    int pool_buffers = 8;
    int packet_queue = 8;
    int rtsp_port = 8554;
    int result_ttl_ms = 300;
    int stall_timeout_ms = 5000;
    int ai_delay_ms = 0;
    float threshold = 0.5f;
    bool ai = true, osd = true;
    std::string iq_dir = "/etc/iqfiles";
    std::string rockx_dir = "/demo/src/rockx_data";

    int output_width() const { return rotation % 180 ? height : width; }
    int output_height() const { return rotation % 180 ? width : height; }
    int sub_output_width() const { return rotation % 180 ? sub_height : sub_width; }
    int sub_output_height() const { return rotation % 180 ? sub_width : sub_height; }

    void validate() const {
        for (int d : {width, height, sub_width, sub_height})
            if (d < 32 || d > 8192 || d % 2)
                throw std::invalid_argument("NV12 image dimensions must be even and in [32,8192]");
        if (sub_width > width || sub_height > height ||
            int64_t(width) * sub_height != int64_t(height) * sub_width)
            throw std::invalid_argument("substream must fit main stream and have the same aspect ratio/FOV");
        if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270)
            throw std::invalid_argument("rotation must be 0,90,180,270");
        if (fps < 1 || fps > 60 || ai_fps < 1 || ai_fps > fps)
            throw std::invalid_argument("require 1 <= ai-fps <= fps <= 60");
        if (pool_buffers < 4 || pool_buffers > 16)
            throw std::invalid_argument("pool-buffers must be in [4,16]");
        if (packet_queue < 4 || packet_queue > 256)
            throw std::invalid_argument("packet-queue must be in [4,256]");
        if (bitrate < 10000 || sub_bitrate < 10000 || rtsp_port < 1 || rtsp_port > 65535 ||
            result_ttl_ms < 1 || result_ttl_ms > 10000 || stall_timeout_ms < 1000 ||
            stall_timeout_ms > 60000 || ai_delay_ms < 0 || ai_delay_ms > 5000 ||
            !std::isfinite(threshold) || threshold < 0 || threshold > 1 || camera < 0 || camera > 7)
            throw std::invalid_argument("invalid rate, port, timeout, confidence or camera setting");
        if (main_node.empty() || sub_node.empty() || main_node == sub_node || (ai && rockx_dir.empty()))
            throw std::invalid_argument("two distinct VI nodes and a RockX model directory are required");
    }
};

inline int parse_integer(const std::string& value) {
    size_t end = 0;
    int result = std::stoi(value, &end);
    if (end != value.size()) throw std::invalid_argument("invalid integer: " + value);
    return result;
}

inline Config parse_config(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--no-ai") { c.ai = false; continue; }
        if (key == "--no-osd") { c.osd = false; continue; }
        if (key == "--no-isp") { c.iq_dir.clear(); continue; }
        if (i + 1 == argc) throw std::invalid_argument("missing value: " + key);
        const std::string value = argv[++i];
        if (key == "--main-node") c.main_node = value;
        else if (key == "--sub-node") c.sub_node = value;
        else if (key == "--iq-dir") c.iq_dir = value;
        else if (key == "--rockx-dir") c.rockx_dir = value;
        else if (key == "--threshold") {
            size_t end = 0;
            c.threshold = std::stof(value, &end);
            if (end != value.size()) throw std::invalid_argument("invalid threshold");
        }
        else if (key == "--camera") c.camera = parse_integer(value);
        else if (key == "--width") c.width = parse_integer(value);
        else if (key == "--height") c.height = parse_integer(value);
        else if (key == "--sub-width") c.sub_width = parse_integer(value);
        else if (key == "--sub-height") c.sub_height = parse_integer(value);
        else if (key == "--rotation") c.rotation = parse_integer(value);
        else if (key == "--fps") c.fps = parse_integer(value);
        else if (key == "--ai-fps") c.ai_fps = parse_integer(value);
        else if (key == "--bitrate") c.bitrate = parse_integer(value);
        else if (key == "--sub-bitrate") c.sub_bitrate = parse_integer(value);
        else if (key == "--pool-buffers") c.pool_buffers = parse_integer(value);
        else if (key == "--packet-queue") c.packet_queue = parse_integer(value);
        else if (key == "--rtsp-port") c.rtsp_port = parse_integer(value);
        else if (key == "--result-ttl-ms") c.result_ttl_ms = parse_integer(value);
        else if (key == "--stall-timeout-ms") c.stall_timeout_ms = parse_integer(value);
        else if (key == "--ai-delay-ms") c.ai_delay_ms = parse_integer(value);
        else throw std::invalid_argument("unknown option: " + key);
    }
    c.validate();
    return c;
}

inline void print_usage(std::ostream& out) {
    out << "RKMedia dual-stream face analytics (H.264 RTSP)\n"
           "  --main-node rkispp_scale0 --sub-node rkispp_scale1 --camera 0\n"
           "  --width 1920 --height 1080 --sub-width 640 --sub-height 360\n"
           "  --rotation 0 --fps 30 --ai-fps 5 --threshold 0.5\n"
           "  --bitrate 8000000 --sub-bitrate 800000 --pool-buffers 8\n"
           "  --packet-queue 8\n"
           "  --rtsp-port 8554 --result-ttl-ms 300 --stall-timeout-ms 5000\n"
           "  --iq-dir /etc/iqfiles --rockx-dir /demo/src/rockx_data\n"
           "  --no-ai --no-osd --no-isp --ai-delay-ms 0 --help\n"
           "NV12 dimensions must be even; nodes must share the same FOV.\n"
           "Paths: /live/main_stream and /live/sub_stream\n";
}
} // namespace vision
