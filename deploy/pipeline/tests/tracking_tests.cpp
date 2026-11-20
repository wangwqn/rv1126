#include "vision/tracking.hpp"
#include "vision/config.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace std::chrono;
constexpr std::uint64_t max_age = 500000;
const auto ttl = milliseconds(500);

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

vision::Detection observation(std::uint64_t pts, std::vector<vision::Box> boxes) {
    vision::Detection d;
    d.pts_us = pts; d.width = 360; d.height = 640;
    d.boxes = std::move(boxes);
    return d;
}

vision::Detection render(vision::DetectionCache& cache, vision::DetectionTracker& tracker,
                         std::uint64_t pts, bool enabled = true) {
    return tracker.project(cache.select(pts, max_age, ttl), pts, max_age, ttl, enabled);
}

void test_pts_matching_and_prediction() {
    vision::DetectionCache cache;
    vision::DetectionTracker tracker;
    cache.publish(observation(1000000, {{20, 40, 80, 100, .9f, 2}}));
    check(render(cache, tracker, 999999).boxes.empty(), "future observation must not draw");
    auto first = render(cache, tracker, 1000000);
    check(first.boxes.size() == 1 && first.boxes[0].left == 20 && !tracker.predicted(),
          "first observation has no invented velocity");
    cache.publish(observation(1200000, {{40, 50, 100, 110, .9f, 2}}));
    // AI is ahead of main video: select historical metadata, not latest().
    check(render(cache, tracker, 1100000).boxes[0].left == 20,
          "main stream selects latest eligible historical PTS");
    auto second = render(cache, tracker, 1200000);
    check(second.boxes[0].left == 40 && !tracker.predicted(), "NPU observation corrects prediction");
    auto between = render(cache, tracker, 1300000);
    check(between.boxes[0].left == 50 && between.boxes[0].top == 55 && tracker.predicted(),
          "box moves between two NPU updates using main PTS");
    check(between.completed_at == second.completed_at,
          "prediction does not renew result wall expiry");
    check(render(cache, tracker, 1300000, false).boxes[0].left == 40 && !tracker.predicted(),
          "no-tracking control retains PTS matching but disables extrapolation");
    check(render(cache, tracker, 1700000).boxes.size() == 1,
          "inclusive PTS time-difference limit");
    check(render(cache, tracker, 1700001).boxes.empty() && !tracker.predicted(),
          "clear immediately beyond PTS difference limit");
    check(render(cache, tracker, 1800000).boxes.empty(), "expired tracks cannot extend themselves");
}

void test_multiple_objects_and_class_association() {
    vision::DetectionCache cache;
    vision::DetectionTracker tracker;
    cache.publish(observation(1000000, {{20, 20, 60, 60, .9f, 1},
                                       {200, 100, 240, 140, .8f, 1},
                                       {80, 300, 120, 340, .7f, 2}}));
    render(cache, tracker, 1000000);
    // Reorder observations and confidence ranks. Tracks must follow geometry.
    cache.publish(observation(1200000, {{180, 100, 220, 140, .95f, 1},
                                       {40, 20, 80, 60, .8f, 1},
                                       {80, 320, 120, 360, .7f, 2}}));
    render(cache, tracker, 1200000);
    auto r = render(cache, tracker, 1300000);
    check(r.boxes.size() == 3 && r.boxes[0].left == 170 && r.boxes[1].left == 50 &&
              r.boxes[2].top == 330, "multiple objects retain independent motion");
    cache.publish(observation(1400000, {{60, 20, 100, 60, .9f, 7}}));
    render(cache, tracker, 1400000);
    check(render(cache, tracker, 1500000).boxes[0].left == 60,
          "different class cannot inherit previous velocity");
    cache.publish(observation(1600000, {}));
    check(render(cache, tracker, 1600000).boxes.empty(), "explicit empty detection clears all tracks");
    cache.publish(observation(1800000, {{80, 20, 120, 60, .9f, 7}}));
    check(render(cache, tracker, 1900000).boxes[0].left == 80,
          "reappearing target cannot resurrect cleared velocity");
}

void test_wall_expiry_resets_and_bad_input() {
    vision::DetectionCache cache;
    vision::DetectionTracker tracker;
    auto old = observation(1000000, {{20, 20, 60, 60, .9f, 1}});
    old.completed_at = steady_clock::now() - seconds(1);
    cache.publish(old);
    check(render(cache, tracker, 1000000).boxes.empty(), "stalled main PTS still obeys wall expiry");
    old.completed_at = steady_clock::now() + hours(1);
    cache.publish(old);
    check(render(cache, tracker, 1000000).boxes.empty(), "future wall completion is invalid");
    cache.publish(observation(1000000, {{20, 20, 60, 60, .9f, 1}}));
    render(cache, tracker, 1000000);
    cache.publish(observation(1200000, {{40, 20, 80, 60, .9f, 1}}));
    render(cache, tracker, 1200000);
    check(render(cache, tracker, 1300000).boxes[0].left == 50, "establish nonzero velocity");
    check(render(cache, tracker, 1100000).boxes[0].left == 20,
          "main PTS regression resets velocity before historical selection");
    render(cache, tracker, 1200000);
    cache.publish(observation(1200000, {{200, 20, 240, 60, .9f, 1}}));
    check(render(cache, tracker, 1300000).boxes[0].left == 200,
          "equal-PTS replacement clears stale velocity");
    cache.publish(observation(2000000, {{220, 20, 260, 60, .9f, 1}}));
    check(render(cache, tracker, 2100000).boxes[0].left == 220,
          "long observation gap cannot reuse velocity");
    auto resized = observation(2200000, {{100, 20, 140, 60, .9f, 1}});
    resized.width = 640; resized.height = 360;
    cache.publish(resized);
    check(render(cache, tracker, 2300000).boxes[0].left == 100,
          "resolution change resets association");
    auto invalid = observation(2400000, {{0, 0, 0, 30, .9f, 1},
        {1, 1, 10, 10, std::numeric_limits<float>::quiet_NaN(), 1},
        {-10, -20, 30, 40, .8f, 1}});
    cache.publish(invalid);
    const auto valid = render(cache, tracker, 2400000);
    check(valid.boxes.size() == 1 && valid.boxes[0].left == 0 && valid.boxes[0].top == 0,
          "invalid rectangles/scores ignored and bounds clipped");
    invalid.width = 0; cache.publish(invalid);
    check(render(cache, tracker, 2400000).boxes.empty(), "invalid image dimensions clear tracks");
}

void test_limits_and_three_fps_cadence() {
    vision::DetectionCache cache;
    vision::DetectionTracker tracker(2);
    for (std::uint64_t frame = 0; frame < 100; ++frame) {
        const auto pts = 1000000 + frame * 33333;
        if (frame % 10 == 0) {
            const int x = 10 + int(frame);
            cache.publish(observation(pts, {{x, 10, x + 40, 50, .9f, 1},
                {200, 200, 250, 250, .8f, 2}, {100, 300, 150, 350, .7f, 3}}));
        }
        const auto output = render(cache, tracker, pts);
        check(output.boxes.size() == 2, "normal 3 FPS updates do not flicker at 30 FPS main PTS");
    }
    // Build rightward motion then predict past the frame edge.
    vision::DetectionCache edge_cache;
    vision::DetectionTracker edge_tracker;
    edge_cache.publish(observation(1000000, {{280, 580, 320, 620, .9f, 1}}));
    render(edge_cache, edge_tracker, 1000000);
    edge_cache.publish(observation(1200000, {{310, 600, 350, 640, .9f, 1}}));
    render(edge_cache, edge_tracker, 1200000);
    auto edge = render(edge_cache, edge_tracker, 1700000);
    check(edge.boxes[0].left == 320 && edge.boxes[0].right == 360 &&
              edge.boxes[0].top == 600 && edge.boxes[0].bottom == 640,
          "predicted whole rectangle is clamped without invalid dimensions");
    check(vision::same_overlay_boxes(edge.boxes, edge.boxes), "stationary overlays can skip updates");
    check(!vision::same_overlay_boxes(edge.boxes, {}), "clear is a visible overlay change");
}

void test_configuration() {
    const char* args[] = {"test", "--result-max-pts-ms", "400", "--no-tracking"};
    auto c = vision::parse_config(4, const_cast<char**>(args));
    check(c.result_max_pts_ms == 400 && !c.tracking && c.result_ttl_ms == 500,
          "source PTS and wall TTL are independently configurable");
    bool rejected = false;
    try { c.result_max_pts_ms = 0; c.validate(); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "zero PTS age is rejected");
    rejected = false;
    try { c.result_max_pts_ms = 10001; c.validate(); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "excessive PTS age is rejected");
}
} // namespace

int main() {
    try {
        test_pts_matching_and_prediction();
        test_multiple_objects_and_class_association();
        test_wall_expiry_resets_and_bad_input();
        test_limits_and_three_fps_cadence();
        test_configuration();
        std::cout << "tracking tests passed\n";
    } catch (const std::exception& e) {
        std::cerr << "tracking tests failed: " << e.what() << '\n';
        return 1;
    }
}
