#include "decode.hpp"
#include <cassert>
#include <iostream>
#include <limits>

int main() {
    constexpr int grid = 4, plane = 16, channels = 21;
    std::vector<float> data(plane * channels, -100.f);
    for (int k = 0; k < 4; ++k) data[k * plane + 5] = 0;
    data[4 * plane + 5] = 10; data[6 * plane + 5] = 10;
    const std::array<float, 6> anchors = {{8, 8, 8, 8, 8, 8}};
    std::vector<vision::Box> boxes;
    object_detection::decode_head(data.data(), grid, 2, false, anchors, 32, .35,
                                  32, 32, 32, 32, 0, 0, boxes);
    assert(boxes.size() == 1 && boxes[0].class_id == 1 && boxes[0].score > .99);
    assert(boxes[0].left == 8 && boxes[0].top == 8 && boxes[0].right == 16 && boxes[0].bottom == 16);
    auto duplicate = boxes[0]; duplicate.score = .8; boxes.push_back(duplicate);
    duplicate.class_id = 0; boxes.push_back(duplicate);
    assert(object_detection::nms(boxes).size() == 2);
    std::vector<vision::Box> kept;
    object_detection::nms_into(boxes, kept);
    assert(kept.size() == 2);
    std::vector<float> nhwc(data.size());
    for (int c = 0; c < channels; ++c) for (int p = 0; p < plane; ++p) nhwc[p * channels + c] = data[c * plane + p];
    boxes.clear();
    object_detection::decode_head(nhwc.data(), grid, 2, true, anchors, 32, .35,
                                  64, 32, 32, 16, 0, 8, boxes);
    assert(boxes.size() == 1 && boxes[0].left == 16 && boxes[0].right == 32);
    assert(boxes[0].top == 0 && boxes[0].bottom == 16);
    vision::Detection d; d.width = 64; d.height = 32; d.boxes = boxes;
    auto scaled = vision::scale_boxes(d, 128, 64);
    assert(scaled.size() == 1 && scaled[0].class_id == 1 && scaled[0].left == 32);
    std::fill(data.begin(), data.end(), std::numeric_limits<float>::quiet_NaN());
    boxes.clear();
    object_detection::decode_head(data.data(), grid, 2, false, anchors, 32, .35,
                                  32, 32, 32, 32, 0, 0, boxes);
    assert(boxes.empty());
    std::cout << "NCHW/NHWC, letterbox, class-aware NMS, class-ID scaling and NaN tests passed\n";
}
