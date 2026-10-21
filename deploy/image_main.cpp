#include "objects.hpp"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <iostream>
#include <chrono>

int main(int argc, char** argv) {
    if (argc != 6 && argc != 7) {
        std::cerr << "Usage: object_image model.rknn model.json labels.png input.jpg output.jpg [threshold]\n";
        return 1;
    }
    try {
        object_detection::Model model(argv[1], argv[2], argc == 7 ? std::stof(argv[6]) : .35f);
        object_detection::LabelPainter painter(argv[3]);
        cv::Mat bgr = cv::imread(argv[4]), rgb;
        if (bgr.empty()) throw std::runtime_error("Cannot read input image");
        cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
        const auto start = std::chrono::steady_clock::now();
        const auto detection = model.infer(rgb);
        std::cout << "inference+pre/postprocess ms=" << std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count() << '\n';
        for (const auto& box : detection.boxes)
            std::cout << model.names()[box.class_id] << " " << box.score << " " << box.left << ','
                      << box.top << ',' << box.right << ',' << box.bottom << '\n';
        painter.draw(bgr, detection.boxes);
        if (!cv::imwrite(argv[5], bgr)) throw std::runtime_error("Cannot write result");
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
}
