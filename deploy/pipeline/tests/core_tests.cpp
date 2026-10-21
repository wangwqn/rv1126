#include "vision/core.hpp"
#include "vision/config.hpp"

#include <atomic>
#include <future>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

vision::Detection detection(std::uint64_t pts, bool empty = false) {
    vision::Detection value;
    value.pts_us = pts;
    value.width = 640;
    value.height = 360;
    if (!empty) {
        value.boxes.push_back({10, 20, 100, 120, 0.9f});
    }
    return value;
}

vision::Config parse(std::initializer_list<std::string> arguments) {
    std::vector<std::string> storage{"rk-vision"};
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (auto& value : storage) argv.push_back(&value[0]);
    return vision::parse_config(static_cast<int>(argv.size()), argv.data());
}

template <typename Function>
void expect_invalid(Function function, const std::string& message) {
    bool rejected = false;
    try {
        function();
    } catch (const std::invalid_argument&) {
        rejected = true;
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    check(rejected, message);
}

void test_config_parsing_and_validation() {
    const auto defaults = parse({});
    check(defaults.fps == 30 && defaults.ai_fps == 3 &&
              defaults.bitrate == 4000000 && defaults.sub_bitrate == 600000 &&
              defaults.sensor_mode == "1080p",
          "board-validated low-latency defaults are preserved");

    const auto configured = parse({"--width", "1920", "--height", "1080",
                                   "--sub-width", "640", "--sub-height", "360",
                                   "--rotation", "90", "--fps", "30",
                                   "--ai-fps", "6", "--rtsp-port", "9554",
                                   "--sensor-mode", "current",
                                   "--threshold", "0.75", "--no-ai", "--no-osd",
                                   "--no-isp"});
    check(configured.width == 1920 && configured.height == 1080 &&
              configured.output_width() == 1080 && configured.output_height() == 1920,
          "rotation swaps configured output dimensions");
    check(configured.fps == 30 && configured.ai_fps == 6 &&
              configured.rtsp_port == 9554 && configured.threshold == 0.75f &&
              configured.sensor_mode == "current",
          "numeric command-line settings are parsed");
    check(!configured.ai && !configured.osd && configured.iq_dir.empty(),
          "boolean disable switches are parsed");

    expect_invalid([] { parse({"--width", "641"}); },
                   "unaligned dimensions are rejected");
    expect_invalid([] { parse({"--sub-width", "640", "--sub-height", "352"}); },
                   "mismatched stream aspect ratio is rejected");
    expect_invalid([] { parse({"--ai-fps", "31"}); },
                   "AI sampling cannot exceed video rate");
    expect_invalid([] { parse({"--threshold", "nan"}); },
                   "non-finite confidence is rejected");
    expect_invalid([] { parse({"--unknown", "1"}); },
                   "unknown options are rejected");
    expect_invalid([] { parse({"--sensor-mode", "720p"}); },
                   "unsupported sensor modes are rejected");
    expect_invalid([] { parse({"--fps"}); },
                   "missing option values are rejected");
    expect_invalid([] { parse({"--bitrate", "999999999999999999999"}); },
                   "integer overflow is rejected");
}

void test_queue_drop_oldest() {
    vision::LatestQueue<int> queue(2);
    check(queue.push(1) && queue.push(2) && queue.push(3), "push accepts open queue");
    check(queue.size() == 2 && queue.dropped() == 1, "bounded oldest eviction");
    int value = 0;
    check(queue.pop(value, milliseconds(0)) && value == 2, "oldest surviving frame");
    check(queue.pop(value, milliseconds(0)) && value == 3, "newest surviving frame");
    check(!queue.pop(value, milliseconds(1)), "empty pop times out");
    check(queue.push(4), "queue reusable after empty");
    queue.close();
    queue.close();
    check(queue.size() == 0 && queue.dropped() == 2, "close purges once");
    check(!queue.push(5), "closed queue rejects producer");
    check(!queue.pop(value, milliseconds(1)), "closed queue does not drain stale frames");

    bool rejected = false;
    try {
        vision::LatestQueue<int> invalid(0);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "zero queue capacity rejected");
}

void test_close_wakes_waiters() {
    vision::LatestQueue<int> queue(2);
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    auto waiter = std::async(std::launch::async, [&] {
        entered.set_value();
        int value = 0;
        return queue.pop(value, seconds(5));
    });
    entered_future.wait();
    std::this_thread::sleep_for(milliseconds(15));
    queue.close();
    const bool woke = waiter.wait_for(milliseconds(300)) == std::future_status::ready;
    const bool popped = waiter.get();
    check(woke && !popped, "close promptly wakes a blocked consumer");
}

void test_lossless_queue_backpressure() {
    vision::BlockingQueue<int> queue(1);
    check(queue.push(1), "lossless queue accepts first item");
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    auto producer = std::async(std::launch::async, [&] {
        entered.set_value();
        return queue.push(2);
    });
    entered_future.wait();
    check(producer.wait_for(milliseconds(20)) == std::future_status::timeout,
          "full queue backpressures producer");
    int value = 0;
    check(queue.pop(value, milliseconds(0)) && value == 1,
          "first item is never evicted");
    check(producer.wait_for(milliseconds(300)) == std::future_status::ready && producer.get(),
          "consumer space wakes producer");
    check(queue.pop(value, milliseconds(0)) && value == 2,
          "backpressured item is preserved");
    check(queue.high_water() == 1 && queue.abandoned() == 0,
          "runtime queue has no drops");
    queue.push(3);
    queue.close();
    check(queue.abandoned() == 1, "shutdown cleanup is accounted separately");
}

void test_shared_leases_and_reentrant_deleters() {
    vision::LatestQueue<std::shared_ptr<int>> queue(1);
    std::atomic<int> releases{0};
    auto lease = [&](int number) {
        return std::shared_ptr<int>(new int(number), [&](int* value) {
            // Hardware release callbacks can reenter diagnostics. This would
            // deadlock if eviction, close, or pop assignment held the mutex.
            static_cast<void>(queue.size());
            static_cast<void>(queue.dropped());
            ++releases;
            delete value;
        });
    };
    auto shared = lease(1);
    queue.push(shared);
    queue.push(lease(2));
    check(releases == 0, "eviction preserves an external shared buffer lease");
    shared.reset();
    check(releases == 1, "last external lease returns buffer exactly once");

    queue.push(lease(3));
    check(releases == 2, "unshared eviction releases outside queue lock");
    auto output = lease(4);
    check(queue.pop(output, milliseconds(0)) && *output == 3, "move lease to consumer");
    check(releases == 3, "pop output replacement releases outside queue lock");
    output.reset();
    check(releases == 4, "consumer returns its lease");
    queue.push(lease(5));
    queue.close();
    check(releases == 5, "shutdown returns queued hardware buffer");
    check(!queue.push(lease(6)) && releases == 6, "rejected push returns buffer");

    vision::LatestQueue<std::unique_ptr<int>> move_only(1);
    check(move_only.push(std::unique_ptr<int>(new int(7))), "move-only producer lease");
    std::unique_ptr<int> unique;
    check(move_only.pop(unique, milliseconds(0)) && *unique == 7,
          "move-only consumer lease");
}

void test_slow_ai_does_not_block_main_stream() {
    vision::LatestQueue<int> ai_frames(2);
    std::promise<void> ai_started;
    std::promise<void> release_ai;
    auto started = ai_started.get_future();
    auto gate = release_ai.get_future();
    std::thread ai([&] {
        int value = 0;
        if (ai_frames.pop(value, seconds(1))) {
            ai_started.set_value();
            gate.wait();  // Model an accelerator that is arbitrarily slow.
        } else {
            ai_started.set_value();
        }
    });
    ai_frames.push(0);
    started.wait();
    auto main_stream = std::async(std::launch::async, [&] {
        for (int frame = 1; frame <= 500; ++frame) {
            ai_frames.push(frame);
        }
    });
    const bool main_completed =
        main_stream.wait_for(seconds(1)) == std::future_status::ready;
    // Always release and join before reporting a failure.
    release_ai.set_value();
    main_stream.get();
    ai.join();
    check(main_completed, "main route keeps producing while AI is blocked");
    check(ai_frames.size() == 2 && ai_frames.dropped() == 498,
          "slow AI has bounded backlog and drops stale frames");
    int value = 0;
    check(ai_frames.pop(value, milliseconds(0)) && value == 499,
          "slow AI resumes near live edge");
    check(ai_frames.pop(value, milliseconds(0)) && value == 500,
          "latest main frame available to AI");
}

void test_detection_history_and_time_limits() {
    vision::DetectionCache cache(3);
    check(cache.publish(detection(100)), "publish first detection");
    check(cache.publish(detection(200, true)), "publish empty clear result");
    check(cache.publish(detection(300)), "publish future detection");
    check(!cache.select(99, 1000, seconds(1)), "never use detection from future PTS");
    auto historical = cache.select(150, 1000, seconds(1));
    check(historical && historical->pts_us == 100, "select corresponding history");
    auto cleared = cache.select(250, 1000, seconds(1));
    check(cleared && cleared->pts_us == 200 && cleared->boxes.empty(),
          "empty result clears previous overlay");
    check(!cache.publish(detection(250)), "late AI completion cannot roll cache back");
    auto current = cache.select(400, 100, seconds(1));
    check(current && current->pts_us == 300, "inclusive PTS age limit");
    check(!cache.select(401, 100, seconds(1)), "PTS age expires detection");
    check(!cache.select(300, 100, milliseconds(-1)), "negative wall age is invalid");

    auto same_pts = detection(300, true);
    check(cache.publish(std::move(same_pts)), "equal PTS replacement accepted");
    check(cache.select(300, 0, seconds(1))->boxes.empty(), "equal PTS replaces contents");
    check(!current->boxes.empty(), "existing immutable lease survives replacement");
    cache.publish(detection(400));
    check(!cache.select(150, 1000, seconds(1)), "capacity bounds historical entries");
    check(historical->pts_us == 100, "selected lease survives cache eviction");

    vision::DetectionCache expired;
    auto old = detection(10);
    old.completed_at = steady_clock::now() - seconds(2);
    expired.publish(std::move(old));
    check(!expired.select(10, 0, milliseconds(10)), "wall age expires stalled AI output");
    check(!expired.latest(milliseconds(10)), "latest view applies wall expiry");
    auto impossible = detection(20);
    impossible.completed_at = steady_clock::now() + hours(1);
    expired.publish(std::move(impossible));
    check(!expired.select(20, 100, seconds(1)), "future completion time rejected");
    check(!expired.latest(seconds(1)), "latest view rejects future completion time");

    vision::DetectionCache wide_pts;
    wide_pts.publish(detection(std::numeric_limits<std::uint64_t>::max() - 10));
    check(static_cast<bool>(wide_pts.select(std::numeric_limits<std::uint64_t>::max(),
                                           10, seconds(1))),
          "PTS subtraction is safe at uint64 limit");
    check(!wide_pts.select(0, std::numeric_limits<std::uint64_t>::max(), seconds(1)),
          "future PTS cannot wrap into an eligible age");

    bool rejected = false;
    try {
        vision::DetectionCache invalid(0);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "zero history capacity rejected");
}

void test_box_scaling_and_bounds() {
    auto input = detection(100, true);
    input.boxes = {
        {1, 3, 639, 359, 0.9f},
        {-100, -100, 1000, 1000, 1.0f},
        {200, 20, 100, 120, 0.9f},
        {10, 20, 100, 20, 0.9f},
        {700, 400, 800, 500, 0.9f},
        {1, 1, 2, 2, std::numeric_limits<float>::quiet_NaN()},
        {1, 1, 2, 2, std::numeric_limits<float>::infinity()},
        {1, 1, 2, 2, -0.1f},
        {1, 1, 2, 2, 1.1f},
    };
    auto boxes = vision::scale_boxes(input, 1920, 1080);
    check(boxes.size() == 2, "discard inverted, degenerate, outside and invalid-score boxes");
    check(boxes[0].left == 2 && boxes[0].top == 8 && boxes[0].right == 1918 &&
              boxes[0].bottom == 1078,
          "scale independently and expand to even NV12 coordinates");
    check(boxes[1].left == 0 && boxes[1].top == 0 && boxes[1].right == 1920 &&
              boxes[1].bottom == 1080,
          "clamp partially visible detection to image bounds");
    check(boxes[0].score == 0.9f, "preserve detection confidence");

    boxes = vision::scale_boxes(input, 641, 361);
    for (const auto& box : boxes) {
        check(box.left >= 0 && box.top >= 0 && box.right <= 640 && box.bottom <= 360 &&
                  box.left % 2 == 0 && box.top % 2 == 0 && box.right % 2 == 0 &&
                  box.bottom % 2 == 0,
              "odd output dimensions still have safe chroma bounds");
    }
    input.width = 0;
    check(vision::scale_boxes(input, 1920, 1080).empty(), "invalid source dimensions");
    input.width = 640;
    check(vision::scale_boxes(input, 1, 1080).empty(), "unrepresentable NV12 dimensions");
    check(vision::scale_boxes(input, -1, 1080).empty(), "negative output dimensions");

    input.width = 10;
    input.height = 10;
    input.boxes = {{std::numeric_limits<int>::min(), std::numeric_limits<int>::min(),
                    std::numeric_limits<int>::max(), std::numeric_limits<int>::max(),
                    0.5f}};
    boxes = vision::scale_boxes(input, std::numeric_limits<int>::max(),
                                 std::numeric_limits<int>::max());
    check(boxes.size() == 1 && boxes[0].left == 0 && boxes[0].top == 0 &&
              boxes[0].right == std::numeric_limits<int>::max() - 1 &&
              boxes[0].bottom == std::numeric_limits<int>::max() - 1,
          "extreme coordinates use safe floating ratio and bounded alignment");
}

}  // namespace

int main() {
    try {
        test_config_parsing_and_validation();
        test_queue_drop_oldest();
        test_close_wakes_waiters();
        test_lossless_queue_backpressure();
        test_shared_leases_and_reentrant_deleters();
        test_slow_ai_does_not_block_main_stream();
        test_detection_history_and_time_limits();
        test_box_scaling_and_bounds();
        std::cout << "All core tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Core test failure: " << error.what() << '\n';
        return 1;
    }
}
