#pragma once
#include "vision/core.hpp"
#include <opencv2/core.hpp>
#include <rknn_api.h>
#include <array>
#include <string>
#include <vector>

namespace object_detection {
class Model {
public:
    Model(const std::string& path, const std::string& metadata, float threshold);
    ~Model();
    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;
    // Synchronous: borrows RGB input only for the duration of this call.
    vision::Detection infer(const cv::Mat& rgb);
    const std::vector<std::string>& names() const { return names_; }
private:
    rknn_context context_ = 0;
    bool initialized_ = false;
    int size_ = 0;
    float threshold_;
    std::vector<std::string> names_;
    std::array<rknn_tensor_attr, 3> outputs_{};
    std::array<int, 3> grids_{};
    std::array<std::array<float, 6>, 3> anchors_{};
    // infer() is synchronous and called by one AI worker. Keep its scratch
    // storage here so every frame does not allocate the letterbox image and
    // repeatedly grow the decoded-candidate array.
    cv::Mat input_;
    int input_source_width_ = 0;
    int input_source_height_ = 0;
    std::vector<vision::Box> candidates_;
};

class LabelPainter {
public:
    explicit LabelPainter(const std::string& atlas);
    // Creates one small BGRA top strip for an RGA ARGB bitmap region. The
    // transparent pixels leave the NV12 video untouched.
    cv::Mat render_top_strip(int width, const vision::Box& box,
                             bool line_at_bottom) const;
    // Offline image-test helper; the real-time RTSP path does not call this.
    void draw(cv::Mat& bgr, const std::vector<vision::Box>& boxes) const;
private:
    cv::Mat atlas_;
    std::vector<int> label_widths_;
};
} // namespace object_detection
