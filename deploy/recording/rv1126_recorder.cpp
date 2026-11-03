#include <gst/gst.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

std::atomic<bool> stopping{false};

void on_signal(int) { stopping.store(true); }

int integer(const char* value, int minimum, int maximum, const char* name) {
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (!end || *end || parsed < minimum || parsed > maximum)
        throw std::invalid_argument(std::string("invalid ") + name);
    return static_cast<int>(parsed);
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::string url = "rtsp://127.0.0.1:8554/live/main_stream";
        std::string location;
        int segment_seconds = 10;
        for (int index = 1; index < argc; ++index) {
            const std::string key = argv[index];
            if (key == "--url" && index + 1 < argc) url = argv[++index];
            else if (key == "--location" && index + 1 < argc) location = argv[++index];
            else if (key == "--segment-seconds" && index + 1 < argc)
                segment_seconds = integer(argv[++index], 5, 600, "segment duration");
            else if (key == "--help") {
                std::cout << "Usage: rv1126-recorder --location FILE-%05d.mkv "
                             "[--url RTSP] [--segment-seconds 10]\n";
                return 0;
            } else throw std::invalid_argument("unknown or incomplete option: " + key);
        }
        if (location.empty() || location.find("%") == std::string::npos)
            throw std::invalid_argument("location must contain a printf sequence number");

        gst_init(&argc, &argv);
        const std::uint64_t segment_ns =
            static_cast<std::uint64_t>(segment_seconds) * GST_SECOND;
        const std::string description =
            "rtspsrc name=source protocols=tcp latency=50 drop-on-latency=true "
            "splitmuxsink name=segments muxer=matroskamux max-size-time=" +
            std::to_string(segment_ns) +
            " max-size-bytes=0 send-keyframe-requests=true "
            "source. ! application/x-rtp,media=video,encoding-name=H264 "
            "! queue max-size-time=2000000000 ! rtph264depay "
            "! h264parse config-interval=-1 ! video/x-h264,stream-format=avc,alignment=au "
            "! queue max-size-time=2000000000 ! segments.video "
            "source. ! application/x-rtp,media=audio,encoding-name=PCMA "
            "! queue max-size-time=2000000000 ! rtppcmadepay "
            "! audio/x-alaw,rate=8000,channels=1 "
            "! queue max-size-time=2000000000 ! segments.audio_0";

        GError* parse_error = nullptr;
        GstElement* pipeline = gst_parse_launch(description.c_str(), &parse_error);
        if (!pipeline) {
            const std::string message = parse_error ? parse_error->message : "unknown error";
            if (parse_error) g_error_free(parse_error);
            throw std::runtime_error("GStreamer pipeline parse failed: " + message);
        }
        if (parse_error) {
            const std::string message = parse_error->message;
            g_error_free(parse_error);
            gst_object_unref(pipeline);
            throw std::runtime_error("GStreamer pipeline parse warning: " + message);
        }
        GstElement* split = gst_bin_get_by_name(GST_BIN(pipeline), "segments");
        GstElement* source_element = gst_bin_get_by_name(GST_BIN(pipeline), "source");
        if (!split || !source_element) {
            if (split) gst_object_unref(split);
            if (source_element) gst_object_unref(source_element);
            gst_object_unref(pipeline);
            throw std::runtime_error("required GStreamer element is unavailable");
        }
        g_object_set(source_element, "location", url.c_str(), nullptr);
        g_object_set(split, "location", location.c_str(), nullptr);

        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        GstBus* bus = gst_element_get_bus(pipeline);
        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
            throw std::runtime_error("cannot start recording pipeline");
        std::cerr << "recording: started url=" << url << " location=" << location
                  << " segment=" << segment_seconds << "s\n";

        bool failed = false;
        while (!stopping.load()) {
            GstMessage* message = gst_bus_timed_pop_filtered(
                bus, GST_SECOND,
                static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
            if (!message) continue;
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                GError* error = nullptr;
                gchar* debug = nullptr;
                gst_message_parse_error(message, &error, &debug);
                std::cerr << "recording: GStreamer error: "
                          << (error ? error->message : "unknown") << '\n';
                if (debug) std::cerr << "recording: debug: " << debug << '\n';
                if (error) g_error_free(error);
                if (debug) g_free(debug);
                failed = true;
            }
            gst_message_unref(message);
            break;
        }

        if (!failed) {
            gst_element_send_event(pipeline, gst_event_new_eos());
            GstMessage* eos = gst_bus_timed_pop_filtered(
                bus, 10 * GST_SECOND,
                static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
            if (eos) gst_message_unref(eos);
        }
        gst_element_set_state(pipeline, GST_STATE_NULL);
        std::cerr << "recording: stopped\n";
        gst_object_unref(source_element);
        gst_object_unref(split);
        gst_object_unref(bus);
        gst_object_unref(pipeline);
        return failed ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "recording: fatal: " << error.what() << '\n';
        return 1;
    }
}
