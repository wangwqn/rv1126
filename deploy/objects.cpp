#include "objects.hpp"
#include "decode.hpp"
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace object_detection {
namespace {
void checked(int status, const char* op) {
    if (status != RKNN_SUCC) throw std::runtime_error(std::string(op) + " failed: " + std::to_string(status));
}
struct OutputLease {
    rknn_context ctx;
    std::array<rknn_output, 3> buffers{};
    bool active = false;
    explicit OutputLease(rknn_context context) : ctx(context) {
        for (unsigned i = 0; i < 3; ++i) { buffers[i].index = i; buffers[i].want_float = 1; }
        checked(rknn_outputs_get(ctx, 3, buffers.data(), nullptr), "rknn_outputs_get");
        active = true;
    }
    ~OutputLease() { if (active) rknn_outputs_release(ctx, 3, buffers.data()); }
    OutputLease(const OutputLease&) = delete;
    OutputLease& operator=(const OutputLease&) = delete;
};
}

Model::Model(const std::string& path, const std::string& metadata, float threshold) : threshold_(threshold) {
    if (!std::isfinite(threshold) || threshold <= 0 || threshold > 1) throw std::runtime_error("Invalid threshold");
    cv::FileStorage fs(metadata, cv::FileStorage::READ);
    if (!fs.isOpened()) throw std::runtime_error("Cannot read model metadata: " + metadata);
    if (int(fs["schema"]) != 1 || std::string(fs["target"]) != "rv1126") throw std::runtime_error("Wrong metadata schema/target");
    size_ = int(fs["size"]);
    if (size_ < 32 || size_ > 1280 || size_ % 32) throw std::runtime_error("Invalid model size");
    for (auto c : fs["classes"]) names_.push_back(std::string(c["zh"]));
    if (names_.empty() || names_.size() > 80) throw std::runtime_error("Invalid class count");
    auto strides = fs["strides"], anchors = fs["anchors"];
    if (strides.size() != 3 || anchors.size() != 3) throw std::runtime_error("Expected three heads in metadata");
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read RKNN model: " + path);
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
    if (bytes.empty() || bytes.size() > 256 * 1024 * 1024) throw std::runtime_error("Invalid model size in bytes");
    checked(rknn_init(&context_, bytes.data(), bytes.size(), 0), "rknn_init");
    initialized_ = true;
    try {
        rknn_sdk_version ver{};
        checked(rknn_query(context_, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver)), "SDK version");
        std::cout << "RKNN runtime=" << ver.api_version << " driver=" << ver.drv_version << '\n';
        rknn_input_output_num count{};
        checked(rknn_query(context_, RKNN_QUERY_IN_OUT_NUM, &count, sizeof(count)), "tensor count");
        if (count.n_input != 1 || count.n_output != 3) throw std::runtime_error("Expected 1 input, 3 raw heads");
        rknn_tensor_attr in{};
        checked(rknn_query(context_, RKNN_QUERY_INPUT_ATTR, &in, sizeof(in)), "input attr");
        // Toolkit 1 reports dimensions in reverse order (unlike Toolkit 2).
        const bool valid_input = in.n_dims == 4 && in.dims[3] == 1 &&
            ((in.fmt == RKNN_TENSOR_NCHW && in.dims[0] == unsigned(size_) && in.dims[1] == unsigned(size_) && in.dims[2] == 3) ||
             (in.fmt == RKNN_TENSOR_NHWC && in.dims[0] == 3 && in.dims[1] == unsigned(size_) && in.dims[2] == unsigned(size_)));
        if (!valid_input) throw std::runtime_error("Model input differs from metadata / Toolkit 1 layout");
        std::array<bool, 3> seen{};
        for (unsigned i = 0; i < 3; ++i) {
            auto& attr = outputs_[i]; attr.index = i;
            checked(rknn_query(context_, RKNN_QUERY_OUTPUT_ATTR, &attr, sizeof(attr)), "output attr");
            std::cout << "output " << i << " fmt=" << attr.fmt << " dims=" << attr.dims[0] << ',' << attr.dims[1]
                      << ',' << attr.dims[2] << ',' << attr.dims[3] << " elements=" << attr.n_elems << '\n';
            int match = -1;
            for (int j = 0; j < 3; ++j) {
                const double stride_value = double(strides[j]);
                int stride = int(std::lround(stride_value));
                if (!std::isfinite(stride_value) || stride <= 0 || size_ % stride)
                    throw std::runtime_error("Invalid stride=" + std::to_string(stride_value) + " size=" + std::to_string(size_));
                int grid = size_ / stride;
                if (attr.n_elems == unsigned(3 * (names_.size() + 5) * grid * grid)) { match = j; grids_[i] = grid; }
            }
            if (match < 0 || seen[match]) throw std::runtime_error("Unexpected/duplicate raw output shape");
            seen[match] = true;
            if (attr.fmt != RKNN_TENSOR_NCHW && attr.fmt != RKNN_TENSOR_NHWC) throw std::runtime_error("Unknown output layout");
            const unsigned g = grids_[i], channels = 3 * (names_.size() + 5);
            const bool shape_ok = (attr.n_dims == 3 || (attr.n_dims == 4 && attr.dims[3] == 1)) &&
                ((attr.fmt == RKNN_TENSOR_NCHW && attr.dims[0] == g && attr.dims[1] == g && attr.dims[2] == channels) ||
                 (attr.fmt == RKNN_TENSOR_NHWC && attr.dims[0] == channels && attr.dims[1] == g && attr.dims[2] == g));
            if (!shape_ok) throw std::runtime_error("Output shape/layout is not a raw YOLO head");
            auto head = anchors[match];
            if (head.size() != 3) throw std::runtime_error("Expected 3 anchors per head");
            for (int a = 0; a < 3; ++a) for (int xy = 0; xy < 2; ++xy) {
                float v = float(head[a][xy]);
                if (!std::isfinite(v) || v <= 0) throw std::runtime_error("Invalid anchor");
                anchors_[i][2 * a + xy] = v;
            }
        }
        input_ = cv::Mat(size_, size_, CV_8UC3, cv::Scalar(114, 114, 114));
        candidates_.reserve(256);
    } catch (...) { rknn_destroy(context_); initialized_ = false; throw; }
}
Model::~Model() { if (initialized_) rknn_destroy(context_); }

vision::Detection Model::infer(const cv::Mat& rgb) {
    if (rgb.empty() || rgb.type() != CV_8UC3) throw std::runtime_error("Expected RGB uint8 image");
    const double gain = std::min(double(size_) / rgb.cols, double(size_) / rgb.rows);
    const int w = int(std::nearbyint(rgb.cols * gain)), h = int(std::nearbyint(rgb.rows * gain));
    const int left = (size_ - w) / 2, top = (size_ - h) / 2;
    if (rgb.cols != input_source_width_ || rgb.rows != input_source_height_) {
        // A changed aspect ratio can expose pixels that belonged to the old
        // ROI, so restore the complete letterbox background only then.
        input_.setTo(cv::Scalar(114, 114, 114));
        input_source_width_ = rgb.cols;
        input_source_height_ = rgb.rows;
    }
    cv::Mat roi = input_(cv::Rect(left, top, w, h));
    cv::resize(rgb, roi, roi.size(), 0, 0, cv::INTER_LINEAR);
    rknn_input tensor{};
    tensor.index = 0; tensor.buf = input_.data; tensor.size = size_ * size_ * 3;
    tensor.type = RKNN_TENSOR_UINT8; tensor.fmt = RKNN_TENSOR_NHWC; tensor.pass_through = 0;
    checked(rknn_inputs_set(context_, 1, &tensor), "rknn_inputs_set");
    checked(rknn_run(context_, nullptr), "rknn_run");
    OutputLease output(context_);
    candidates_.clear();
    for (unsigned i = 0; i < 3; ++i) {
        if (!output.buffers[i].buf || output.buffers[i].size < outputs_[i].n_elems * sizeof(float))
            throw std::runtime_error("Truncated RKNN output");
        decode_head(static_cast<const float*>(output.buffers[i].buf), grids_[i], names_.size(),
                    outputs_[i].fmt == RKNN_TENSOR_NHWC, anchors_[i], size_, threshold_,
                    rgb.cols, rgb.rows, w, h, left, top, candidates_);
    }
    vision::Detection result;
    result.width = rgb.cols; result.height = rgb.rows;
    nms_into(candidates_, result.boxes);
    result.completed_at = std::chrono::steady_clock::now();
    return result;
}

LabelPainter::LabelPainter(const std::string& atlas) : atlas_(cv::imread(atlas)) {
    if (atlas_.empty() || atlas_.cols != 224 || atlas_.rows % 32)
        throw std::runtime_error("Invalid Chinese label atlas: " + atlas);
    for (int row = 0; row < atlas_.rows / 32; ++row) {
        cv::Mat mask;
        cv::inRange(atlas_(cv::Rect(0, row * 32, 224, 32)), cv::Scalar(100, 100, 100), cv::Scalar(255, 255, 255), mask);
        std::vector<cv::Point> pixels;
        cv::findNonZero(mask, pixels);
        if (pixels.empty()) throw std::runtime_error("Blank label in atlas");
        const auto bounds = cv::boundingRect(pixels);
        label_widths_.push_back(std::min(224, bounds.x + bounds.width + 6));
    }
}
cv::Mat LabelPainter::render_top_strip(int width, const vision::Box& box,
                                       bool line_at_bottom) const {
    if (width <= 0 || box.class_id < 0 || box.class_id >= atlas_.rows / 32)
        throw std::runtime_error("Invalid label strip request");
    constexpr int height = 32;
    cv::Mat strip(height, width, CV_8UC4, cv::Scalar(0, 0, 0, 0));
    const int label_w = std::min(label_widths_[box.class_id], width);
    const int panel_w = std::min(width, label_w + 76);
    strip(cv::Rect(0, 0, panel_w, height)).setTo(cv::Scalar(35, 35, 35, 255));
    if (label_w > 0) {
        cv::Mat label_bgra;
        cv::cvtColor(atlas_(cv::Rect(0, box.class_id * 32, label_w, height)),
                     label_bgra, cv::COLOR_BGR2BGRA);
        label_bgra.copyTo(strip(cv::Rect(0, 0, label_w, height)));
    }
    if (panel_w > label_w + 6)
        cv::putText(strip, std::to_string(int(std::lround(box.score * 100))) + "%",
                    cv::Point(label_w + 2, 24), cv::FONT_HERSHEY_SIMPLEX, .65,
                    cv::Scalar(255, 255, 255, 255), 1, cv::LINE_AA);
    const int line_y = line_at_bottom ? height - 4 : 0;
    cv::rectangle(strip, cv::Rect(0, line_y, width, 4),
                  cv::Scalar(0, 255, 0, 255), cv::FILLED);
    return strip;
}
cv::Mat LabelPainter::render_box_overlay(int width, int height,
                                         const vision::Box& box,
                                         bool label_above) const {
    if (width <= 0 || height <= 0)
        throw std::runtime_error("Invalid box overlay dimensions");
    constexpr int label_height = 32;
    const int box_y = label_above ? label_height : 0;
    const int overlay_height = label_above ? height + label_height
                                           : std::max(height, label_height);
    cv::Mat overlay(overlay_height, width, CV_8UC4,
                    cv::Scalar(0, 0, 0, 0));
    const int thickness = std::min(4, std::min(width, height));
    const cv::Scalar green(0, 255, 0, 255);
    cv::rectangle(overlay, cv::Rect(0, box_y, width, thickness), green,
                  cv::FILLED);
    cv::rectangle(overlay,
                  cv::Rect(0, box_y + height - thickness, width, thickness),
                  green, cv::FILLED);
    cv::rectangle(overlay, cv::Rect(0, box_y, thickness, height), green,
                  cv::FILLED);
    cv::rectangle(overlay,
                  cv::Rect(width - thickness, box_y, thickness, height), green,
                  cv::FILLED);
    render_top_strip(width, box, label_above)
        .copyTo(overlay(cv::Rect(0, 0, width, label_height)));
    return overlay;
}
void LabelPainter::draw(cv::Mat& bgr, const std::vector<vision::Box>& boxes) const {
    for (const auto& box : boxes) {
        if (box.class_id < 0 || box.class_id >= atlas_.rows / 32)
            throw std::runtime_error("Class ID exceeds label atlas");
        cv::Rect rect(box.left, box.top, box.right - box.left, box.bottom - box.top);
        rect &= cv::Rect(0, 0, bgr.cols, bgr.rows);
        if (rect.empty()) continue;
        cv::rectangle(bgr, rect, cv::Scalar(0, 255, 0), 2);
        const int label_w = label_widths_[box.class_id];
        const int width = std::min(label_w + 76, bgr.cols);
        const int height = std::min(32, bgr.rows);
        const int x = std::max(0, std::min(box.left, bgr.cols - width));
        int y = box.top >= height ? box.top - height : box.top;
        y = std::max(0, std::min(y, bgr.rows - height));
        cv::Mat panel = bgr(cv::Rect(x, y, width, height));
        panel.setTo(cv::Scalar(35, 35, 35));
        const int text_width = std::min(label_w, width);
        atlas_(cv::Rect(0, box.class_id * 32, text_width, height))
            .copyTo(panel(cv::Rect(0, 0, text_width, height)));
        if (width > label_w + 6)
            cv::putText(panel,
                        std::to_string(int(std::lround(box.score * 100))) + "%",
                        cv::Point(label_w + 2, 24), cv::FONT_HERSHEY_SIMPLEX, .65,
                        cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
    }
}
} // namespace object_detection
