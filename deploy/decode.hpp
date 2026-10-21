#pragma once
#include "vision/core.hpp"
#include <array>

namespace object_detection {
inline float sigmoid(float x) { return 1.f / (1.f + std::exp(-x)); }

inline void decode_head(const float* values, int grid, int classes, bool nhwc,
                        const std::array<float, 6>& anchors, int input_size,
                        float threshold, int source_w, int source_h,
                        int resized_w, int resized_h, int pad_x, int pad_y,
                        std::vector<vision::Box>& boxes) {
    const int no = classes + 5, plane = grid * grid, channels = 3 * no;
    const float stride = float(input_size) / grid;
    auto at = [&](int a, int k, int pixel) {
        return values[nhwc ? pixel * channels + a * no + k : (a * no + k) * plane + pixel];
    };
    for (int a = 0; a < 3; ++a) for (int p = 0; p < plane; ++p) {
        const float objectness = sigmoid(at(a, 4, p));
        if (!std::isfinite(objectness) || objectness < threshold) continue;
        float best = 0; int cls = -1;
        for (int c = 0; c < classes; ++c) {
            const float score = objectness * sigmoid(at(a, c + 5, p));
            if (score > best) { best = score; cls = c; }
        }
        if (cls < 0 || best < threshold) continue;
        const float cx = (2 * sigmoid(at(a, 0, p)) - .5f + p % grid) * stride;
        const float cy = (2 * sigmoid(at(a, 1, p)) - .5f + p / grid) * stride;
        const float sw = 2 * sigmoid(at(a, 2, p)), sh = 2 * sigmoid(at(a, 3, p));
        const float w = sw * sw * anchors[a * 2], h = sh * sh * anchors[a * 2 + 1];
        if (!std::isfinite(cx + cy + w + h)) continue;
        auto x = [&](float v) { return std::max(0.f, std::min(float(source_w), (v - pad_x) * source_w / resized_w)); };
        auto y = [&](float v) { return std::max(0.f, std::min(float(source_h), (v - pad_y) * source_h / resized_h)); };
        vision::Box b;
        b.left = int(std::floor(x(cx - w / 2))); b.right = int(std::ceil(x(cx + w / 2)));
        b.top = int(std::floor(y(cy - h / 2))); b.bottom = int(std::ceil(y(cy + h / 2)));
        b.score = best; b.class_id = cls;
        if (b.left < b.right && b.top < b.bottom) boxes.push_back(b);
    }
}

inline std::vector<vision::Box> nms(std::vector<vision::Box> boxes, float threshold = .45f) {
    std::sort(boxes.begin(), boxes.end(), [](const vision::Box& a, const vision::Box& b) { return a.score > b.score; });
    std::vector<vision::Box> kept;
    for (const auto& b : boxes) {
        bool suppress = false;
        for (const auto& a : kept) {
            if (a.class_id != b.class_id) continue;
            const float intersection = float(std::max(0, std::min(a.right, b.right) - std::max(a.left, b.left))) *
                                       std::max(0, std::min(a.bottom, b.bottom) - std::max(a.top, b.top));
            const float area = float(a.right - a.left) * (a.bottom - a.top) +
                               float(b.right - b.left) * (b.bottom - b.top) - intersection;
            if (area > 0 && intersection / area > threshold) { suppress = true; break; }
        }
        if (!suppress) kept.push_back(b);
        if (kept.size() >= 100) break;
    }
    return kept;
}
} // namespace object_detection
