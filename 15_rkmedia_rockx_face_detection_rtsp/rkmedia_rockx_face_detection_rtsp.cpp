#include "rkmedia_rockx_face_detection_rtsp.h"

#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <thread>

namespace {
volatile std::sig_atomic_t stop_requested = 0;
extern "C" void request_stop(int) { stop_requested = 1; }
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            vision::print_usage(std::cout);
            return 0;
        }
        auto config = vision::parse_config(argc, argv);
        std::signal(SIGINT, request_stop);
        std::signal(SIGTERM, request_stop);
#ifdef SIGPIPE
        // A client disconnect must be reported by the RTSP stack, not kill the process.
        std::signal(SIGPIPE, SIG_IGN);
#endif

        vision::Pipeline pipeline(std::move(config));
        pipeline.start();
        auto next_statistics = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!stop_requested && !pipeline.failed()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (std::chrono::steady_clock::now() >= next_statistics) {
                std::cout << pipeline.statistics() << std::endl;
                next_statistics += std::chrono::seconds(5);
            }
        }
        pipeline.stop();

        std::cout << pipeline.statistics() << '\n';
        if (pipeline.failed()) {
            std::cerr << "pipeline failed: " << pipeline.error() << '\n';
            return 2;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "startup failed: " << error.what() << '\n';
        vision::print_usage(std::cerr);
        return 1;
    }
}
