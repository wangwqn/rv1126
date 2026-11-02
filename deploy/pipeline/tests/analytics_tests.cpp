#include "vision/analytics.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

vision::Detection frame(std::uint64_t pts, double center_x, int class_id = 0) {
    vision::Detection value;
    value.pts_us = pts;
    value.width = 1000;
    value.height = 500;
    const int x = static_cast<int>(center_x * value.width);
    value.boxes.push_back({x - 30, 200, x + 30, 300, .9f, class_id});
    return value;
}

void test_region_and_line_events_are_async_and_debounced() {
    const std::string base = "/tmp/rv1126-analytics-test-" + std::to_string(getpid());
    const std::string rules = base + ".rules";
    const std::string events = base + ".jsonl";
    {
        std::ofstream output(rules);
        // UTF-8 names "门口" and "中线" encoded as hex.
        output << "region e997a8e58fa3 0.2 0.2 0.6 0.6\n";
        output << "line e4b8ade7babf 0.5 0 0.5 1\n";
    }
    {
        vision::AnalyticsEngine engine(rules, events, {"测试物体"}, 500, 3);
        check(engine.enabled(), "valid rules enable analytics");
        engine.process(frame(1000000, .10));
        engine.process(frame(2000000, .25)); // region enter
        engine.process(frame(3000000, .45));
        engine.process(frame(4000000, .55)); // line cross
        engine.process(frame(5000000, .75));
        engine.process(frame(6000000, .85)); // region exit
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const auto stats = engine.snapshot();
        check(stats.processed == 6 && stats.generated == 3,
              "one enter, crossing and exit are generated");
        check(stats.regions == 1 && stats.lines == 1 && stats.tracks == 1,
              "rule and track counts are reported");
        check(stats.write_errors == 0, "event writer remains healthy");
    }
    std::ifstream input(events);
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    check(text.find("\"type\":\"region_enter\"") != std::string::npos,
          "region enter is persisted");
    check(text.find("\"type\":\"line_cross\"") != std::string::npos,
          "line crossing is persisted");
    check(text.find("\"type\":\"region_exit\"") != std::string::npos,
          "region exit is persisted");
    check(text.find("测试物体") != std::string::npos,
          "UTF-8 class label is preserved");
    std::remove(rules.c_str());
    std::remove(events.c_str());
}

void test_invalid_rule_is_rejected() {
    const std::string base = "/tmp/rv1126-analytics-invalid-" + std::to_string(getpid());
    const std::string rules = base + ".rules";
    { std::ofstream output(rules); output << "line 41 0.5 0.5 0.5 0.5\n"; }
    bool rejected = false;
    try { vision::AnalyticsEngine engine(rules, base + ".jsonl", {}, 1000, 3); }
    catch (const std::runtime_error&) { rejected = true; }
    check(rejected, "degenerate line is rejected");
    std::remove(rules.c_str());
}

} // namespace

int main() {
    try {
        test_region_and_line_events_are_async_and_debounced();
        test_invalid_rule_is_rejected();
        std::cout << "analytics tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "analytics test failed: " << error.what() << '\n';
        return 1;
    }
}
