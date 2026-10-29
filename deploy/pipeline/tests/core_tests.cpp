#include "vision/core.hpp"
#include "vision/config.hpp"
#include "vision/audio.hpp"
#include "vision/latency_trace.hpp"

#include <atomic>
#include <cstdio>
#include <fstream>
#include <future>
#include <initializer_list>
#include <iostream>
#include <iterator>
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
              defaults.max_osd_boxes == 8 && defaults.result_ttl_ms == 500 &&
              defaults.rtsp_client_buffer_kb == 512 &&
              defaults.sensor_mode == "1080p" && defaults.audio &&
              defaults.audio_device == "plughw:0,0" && defaults.audio_period_ms == 20,
          "board-validated low-latency defaults are preserved");

    const auto configured = parse({"--width", "1920", "--height", "1080",
                                   "--sub-width", "640", "--sub-height", "360",
                                   "--rotation", "90", "--fps", "30",
                                   "--ai-fps", "6", "--rtsp-port", "9554",
                                   "--rtsp-client-buffer-kb", "1024",
                                   "--sensor-mode", "current",
                                   "--audio-source", "hands-free",
                                   "--audio-period-ms", "40",
                                   "--max-osd-boxes", "6",
                                   "--threshold", "0.75", "--no-ai", "--no-osd",
                                   "--no-audio", "--no-isp",
                                   "--latency-csv", "/tmp/latency.csv"});
    check(configured.width == 1920 && configured.height == 1080 &&
              configured.output_width() == 1080 && configured.output_height() == 1920,
          "rotation swaps configured output dimensions");
    check(configured.fps == 30 && configured.ai_fps == 6 &&
              configured.rtsp_port == 9554 && configured.threshold == 0.75f &&
              configured.rtsp_client_buffer_kb == 1024 &&
              configured.max_osd_boxes == 6 &&
              configured.sensor_mode == "current" &&
              configured.audio_source == "hands-free" && configured.audio_period_ms == 40,
          "numeric command-line settings are parsed");

    const auto full_fov = parse({"--sensor-mode", "4k-ispp-1080"});
    check(full_fov.sensor_mode == "4k-ispp-1080",
          "full-FOV ISP-to-ISPP mode is accepted");
    check(!configured.ai && !configured.osd && !configured.audio && configured.iq_dir.empty() &&
              configured.latency_stats && configured.latency_csv == "/tmp/latency.csv",
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
    expect_invalid([] { parse({"--sensor-mode", "4k-ispp-1080", "--fps", "29"}); },
                   "offline full-FOV topology requires 30 FPS");
    expect_invalid([] { parse({"--sensor-mode", "4k-ispp-1080", "--no-isp"}); },
                   "offline full-FOV topology requires RKAiq");
    expect_invalid([] { parse({"--fps"}); },
                   "missing option values are rejected");
    expect_invalid([] { parse({"--latency-csv", ""}); },
                   "empty latency CSV path is rejected");
    expect_invalid([] { parse({"--audio-source", "bluetooth"}); },
                   "unsupported audio source is rejected");
    expect_invalid([] { parse({"--audio-period-ms", "7"}); },
                   "too-small PCMA packets are rejected");
    expect_invalid([] { parse({"--max-osd-boxes", "9"}); },
                   "RGA OSD region capacity is enforced");
    expect_invalid([] { parse({"--rtsp-client-buffer-kb", "64"}); },
                   "too-small RTSP client buffers are rejected");
    expect_invalid([] { parse({"--bitrate", "999999999999999999999"}); },
                   "integer overflow is rejected");
}

void test_g711a_encoding_and_downmix() {
    check(vision::linear_to_alaw(0) == 0xd5 &&
              vision::linear_to_alaw(-1) == 0x55 &&
              vision::linear_to_alaw(1000) == 0xfa &&
              vision::linear_to_alaw(-1000) == 0x7a &&
              vision::linear_to_alaw(32767) == 0xaa &&
              vision::linear_to_alaw(-32768) == 0x2a,
          "G.711A matches reference code words");
    const std::int16_t stereo[] = {1000, 1000, 2000, 0, -1000, -1000};
    std::vector<std::uint8_t> encoded;
    vision::encode_g711a_mono(stereo, 3, 2, encoded);
    check(encoded.size() == 3 && encoded[0] == 0xfa && encoded[1] == 0xfa &&
              encoded[2] == 0x7a,
          "stereo PCM is averaged to mono before G.711A encoding");
}

void test_latency_percentiles_and_rolling_window() {
    vision::LatencyWindow values(100);
    for (std::uint64_t milliseconds = 1; milliseconds <= 100; ++milliseconds)
        values.observe_us(milliseconds * 1000);
    auto snapshot = values.snapshot();
    check(snapshot.samples == 100 && snapshot.window == 100,
          "latency sample and window counts are reported");
    check(snapshot.p50_us == 50000 && snapshot.p95_us == 95000 &&
              snapshot.p99_us == 99000 && snapshot.max_us == 100000,
          "nearest-rank latency percentiles are exact");

    vision::LatencyWindow rolling(3);
    rolling.observe_us(1);
    rolling.observe_us(2);
    rolling.observe_us(3);
    rolling.observe_us(4);
    snapshot = rolling.snapshot();
    check(snapshot.samples == 4 && snapshot.window == 3 && snapshot.p50_us == 3 &&
              snapshot.p95_us == 4 && snapshot.max_us == 4,
          "rolling latency window evicts only its oldest sample");
}

void test_async_latency_csv_drains_on_close() {
    const auto unique = steady_clock::now().time_since_epoch().count();
    const std::string path =
        "/tmp/rv1126-latency-writer-test-" + std::to_string(unique) + ".csv";
    {
        vision::AsyncLatencyCsv writer(path, 4096);
        const auto now = steady_clock::now();
        for (std::uint64_t index = 0; index < 1000; ++index) {
            writer.push(index % 2 ? vision::LatencyMetric::ai_infer
                                  : vision::LatencyMetric::main_server_total,
                        now + microseconds(index), 1000000 + index, 15000 + index);
        }
        writer.close();
        writer.close();
        check(writer.written() == 1000 && writer.dropped() == 0 && !writer.failed() &&
                  writer.queued() == 0 && writer.high_water() > 0,
              "asynchronous CSV writer drains every queued sample on close");
    }

    std::ifstream input(path);
    check(static_cast<bool>(input), "latency CSV can be reopened");
    std::string contents((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
    input.close();
    std::remove(path.c_str());
    check(contents.find(
              "observed_monotonic_us,frame_pts_us,stage,latency_us,latency_ms\n") == 0,
          "latency CSV has a stable schema");
    check(contents.find("ai.infer") != std::string::npos &&
              contents.find("main.server_total") != std::string::npos &&
              std::count(contents.begin(), contents.end(), '\n') == 1001,
          "latency CSV stores every stage record exactly once");
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

void test_detection_ttl_covers_three_fps_jitter() {
    const auto ttl = milliseconds(500);

    vision::DetectionCache fresh;
    auto delayed_result = detection(100);
    delayed_result.completed_at = steady_clock::now() - milliseconds(400);
    check(fresh.publish(std::move(delayed_result)), "publish delayed 3 FPS result");
    check(static_cast<bool>(fresh.latest(ttl)),
          "overlay survives one 3 FPS interval plus scheduling jitter");

    vision::DetectionCache stale;
    auto expired_result = detection(100);
    expired_result.completed_at = steady_clock::now() - milliseconds(600);
    check(stale.publish(std::move(expired_result)), "publish stale result");
    check(!stale.latest(ttl), "overlay clears when AI really stops updating");

    vision::DetectionCache explicit_clear;
    check(explicit_clear.publish(detection(100)), "publish visible overlay");
    check(explicit_clear.publish(detection(200, true)), "publish empty detection");
    const auto cleared = explicit_clear.latest(ttl);
    check(cleared && cleared->boxes.empty(),
          "empty AI result clears immediately without waiting for TTL");
}

void test_detection_change_notification() {
    vision::DetectionCache cache;
    std::uint64_t generation = 0;
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    auto waiter = std::async(std::launch::async, [&] {
        entered.set_value();
        return cache.wait_for_change(generation, seconds(1));
    });
    entered_future.wait();
    std::this_thread::sleep_for(milliseconds(15));
    check(cache.publish(detection(100)), "notification test publishes detection");
    check(waiter.wait_for(milliseconds(300)) == std::future_status::ready && waiter.get() &&
              generation == 1,
          "new detection wakes OSD without polling");
    check(!cache.wait_for_change(generation, milliseconds(10)),
          "unchanged detection waits until timeout");
    check(cache.publish(detection(100, true)) &&
              cache.wait_for_change(generation, milliseconds(0)) && generation == 2,
          "equal-PTS replacement increments notification generation");

    std::promise<void> close_entered;
    auto close_entered_future = close_entered.get_future();
    auto closing_waiter = std::async(std::launch::async, [&] {
        close_entered.set_value();
        return cache.wait_for_change(generation, seconds(5));
    });
    close_entered_future.wait();
    std::this_thread::sleep_for(milliseconds(15));
    cache.close();
    check(closing_waiter.wait_for(milliseconds(300)) == std::future_status::ready &&
              !closing_waiter.get(),
          "cache close promptly wakes OSD waiter");
    check(!cache.publish(detection(200)), "closed cache rejects late AI result");
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
        test_g711a_encoding_and_downmix();
        test_latency_percentiles_and_rolling_window();
        test_async_latency_csv_drains_on_close();
        test_queue_drop_oldest();
        test_close_wakes_waiters();
        test_lossless_queue_backpressure();
        test_shared_leases_and_reentrant_deleters();
        test_slow_ai_does_not_block_main_stream();
        test_detection_history_and_time_limits();
        test_detection_ttl_covers_three_fps_jitter();
        test_detection_change_notification();
        test_box_scaling_and_bounds();
        std::cout << "All core tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Core test failure: " << error.what() << '\n';
        return 1;
    }
}
