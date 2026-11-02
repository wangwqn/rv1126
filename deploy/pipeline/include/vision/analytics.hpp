#pragma once

#include "vision/core.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace vision {

struct AnalyticsSnapshot {
    std::uint64_t processed = 0;
    std::uint64_t generated = 0;
    std::uint64_t written = 0;
    std::uint64_t dropped = 0;
    std::uint64_t write_errors = 0;
    std::size_t tracks = 0;
    std::size_t regions = 0;
    std::size_t lines = 0;
};

// Lightweight centroid tracking and rule evaluation. process() is called only
// by the AI worker; event disk I/O is isolated on a bounded, non-blocking queue.
class AnalyticsEngine {
public:
    AnalyticsEngine(const std::string& rules_path, const std::string& event_log,
                    std::vector<std::string> class_names,
                    int cooldown_ms = 2000, int max_missed = 3);
    ~AnalyticsEngine();
    AnalyticsEngine(const AnalyticsEngine&) = delete;
    AnalyticsEngine& operator=(const AnalyticsEngine&) = delete;

    bool enabled() const noexcept;
    void process(const Detection& detection);
    AnalyticsSnapshot snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vision
