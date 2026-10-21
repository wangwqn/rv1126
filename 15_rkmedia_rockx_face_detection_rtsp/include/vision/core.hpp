#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace vision {

// A producer never waits for a consumer: saturation retires the oldest item.
// T may be move-only, including an owning hardware-buffer lease. Its destructor
// and move assignment are always invoked outside the queue's mutex.
template <typename T>
class LatestQueue {
public:
    explicit LatestQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("LatestQueue capacity must be positive");
        }
    }

    ~LatestQueue() { close(); }
    LatestQueue(const LatestQueue&) = delete;
    LatestQueue& operator=(const LatestQueue&) = delete;

    bool push(T value) {
        std::unique_ptr<T> incoming(new T(std::move(value)));
        std::unique_ptr<T> retired;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                return false;
            }
            if (items_.size() == capacity_) {
                retired = std::move(items_.front());
                items_.pop_front();
                ++dropped_;
            }
            items_.push_back(std::move(incoming));
        }
        ready_.notify_one();
        return true;
    }

    // False means timeout or shutdown. close() deliberately discards pending
    // frames, so shutdown cannot accidentally process an obsolete backlog.
    bool pop(T& value, std::chrono::milliseconds timeout) {
        std::unique_ptr<T> next;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!ready_.wait_for(lock, timeout,
                                 [this] { return closed_ || !items_.empty(); })) {
                return false;
            }
            if (closed_) {
                return false;
            }
            next = std::move(items_.front());
            items_.pop_front();
        }
        value = std::move(*next);
        return true;
    }

    void close() {
        std::deque<std::unique_ptr<T>> retired;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
            dropped_ += static_cast<std::uint64_t>(items_.size());
            retired.swap(items_);
        }
        ready_.notify_all();
    }

    // Includes capacity evictions and unconsumed items discarded by close().
    // Rejected pushes after close() are not included.
    std::uint64_t dropped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::unique_ptr<T>> items_;
    std::uint64_t dropped_ = 0;
    bool closed_ = false;
};

// Lossless bounded queue for the video/encoded-packet path. Producers apply
// backpressure instead of evicting frames. close() wakes both producers and
// consumers, which makes an otherwise blocked hardware worker stoppable.
template <typename T>
class BlockingQueue {
public:
    explicit BlockingQueue(std::size_t capacity) : capacity_(capacity) {
        if (!capacity_) throw std::invalid_argument("BlockingQueue capacity must be positive");
    }

    ~BlockingQueue() { close(); }
    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;

    bool push(T value) {
        std::unique_lock<std::mutex> lock(mutex_);
        space_.wait(lock, [this] { return closed_ || items_.size() < capacity_; });
        if (closed_) return false;
        items_.emplace_back(std::move(value));
        high_water_ = std::max(high_water_, items_.size());
        lock.unlock();
        ready_.notify_one();
        return true;
    }

    bool pop(T& value, std::chrono::milliseconds timeout) {
        std::unique_ptr<T> next;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!ready_.wait_for(lock, timeout,
                                 [this] { return closed_ || !items_.empty(); })) return false;
            if (items_.empty()) return false;
            next.reset(new T(std::move(items_.front())));
            items_.pop_front();
        }
        space_.notify_one();
        value = std::move(*next);
        return true;
    }

    void close() {
        std::deque<T> retired;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) return;
            closed_ = true;
            abandoned_ += items_.size();
            retired.swap(items_);
        }
        ready_.notify_all();
        space_.notify_all();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }
    std::size_t high_water() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return high_water_;
    }
    std::uint64_t abandoned() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return abandoned_;
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable ready_, space_;
    std::deque<T> items_;
    std::size_t high_water_ = 0;
    std::uint64_t abandoned_ = 0;
    bool closed_ = false;
};

struct Box {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    float score = 0.0f;
};

struct Detection {
    std::uint64_t pts_us = 0;
    int width = 0;
    int height = 0;
    std::vector<Box> boxes;
    std::chrono::steady_clock::time_point completed_at =
        std::chrono::steady_clock::now();
};

// Small immutable metadata history shared by the AI and the main video path.
// PTS values must use the same monotonic microsecond clock on both streams.
class DetectionCache {
public:
    explicit DetectionCache(std::size_t capacity = 8) : capacity_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("DetectionCache capacity must be positive");
        }
    }

    DetectionCache(const DetectionCache&) = delete;
    DetectionCache& operator=(const DetectionCache&) = delete;

    // Returns false for an out-of-order completion. Equal PTS replaces the
    // matching entry; an empty boxes vector is a valid explicit clear result.
    bool publish(Detection detection) {
        std::shared_ptr<const Detection> incoming =
            std::make_shared<const Detection>(std::move(detection));
        std::shared_ptr<const Detection> retired;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!history_.empty() && incoming->pts_us < history_.back()->pts_us) {
                return false;
            }
            if (!history_.empty() && incoming->pts_us == history_.back()->pts_us) {
                retired = std::move(history_.back());
                history_.back() = std::move(incoming);
            } else {
                if (history_.size() == capacity_) {
                    retired = std::move(history_.front());
                    history_.pop_front();
                }
                history_.push_back(std::move(incoming));
            }
        }
        return true;
    }

    std::shared_ptr<const Detection> select(
        std::uint64_t main_pts_us, std::uint64_t max_age_us,
        std::chrono::milliseconds max_wall_age) const {
        if (max_wall_age.count() < 0) {
            return {};
        }
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
            const auto& detection = *it;
            if (detection->pts_us > main_pts_us) {
                continue;
            }
            // Subtract only after the ordering check to avoid unsigned wrap.
            // Never fall back past an expired/empty latest eligible result and
            // resurrect an older overlay after the AI has cleared it.
            if (main_pts_us - detection->pts_us > max_age_us ||
                detection->completed_at > now ||
                now - detection->completed_at > max_wall_age) {
                return {};
            }
            return detection;
        }
        return {};
    }

    std::shared_ptr<const Detection> latest(
        std::chrono::milliseconds max_wall_age) const {
        if (max_wall_age.count() < 0) return {};
        std::lock_guard<std::mutex> lock(mutex_);
        if (history_.empty()) return {};
        const auto now = std::chrono::steady_clock::now();
        const auto& detection = history_.back();
        if (detection->completed_at > now || now - detection->completed_at > max_wall_age)
            return {};
        return detection;
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<std::shared_ptr<const Detection>> history_;
};

// Convert model/input coordinates to the main NV12 image. Rectangles use
// exclusive right/bottom edges, expand outward to even chroma coordinates,
// and remain inside the largest even-sized region of the output image.
inline std::vector<Box> scale_boxes(const Detection& detection, int out_w,
                                    int out_h) {
    std::vector<Box> result;
    if (detection.width <= 0 || detection.height <= 0 || out_w < 2 || out_h < 2) {
        return result;
    }
    const int bound_w = out_w - out_w % 2;
    const int bound_h = out_h - out_h % 2;
    const double scale_x = static_cast<double>(out_w) / detection.width;
    const double scale_y = static_cast<double>(out_h) / detection.height;
    const auto edge = [](int coordinate, double scale, int bound, bool upper) {
        double mapped = static_cast<double>(coordinate) * scale;
        mapped = std::max(0.0, std::min(static_cast<double>(bound), mapped));
        int aligned = static_cast<int>(upper ? std::ceil(mapped) : std::floor(mapped));
        if (upper && aligned % 2 != 0) {
            ++aligned;  // bound is even, so this cannot overflow or exceed it.
        } else if (!upper) {
            aligned -= aligned % 2;
        }
        return aligned;
    };
    result.reserve(detection.boxes.size());
    for (const auto& box : detection.boxes) {
        if (box.left >= box.right || box.top >= box.bottom ||
            !std::isfinite(box.score) || box.score < 0.0f || box.score > 1.0f) {
            continue;
        }
        Box scaled;
        scaled.left = edge(box.left, scale_x, bound_w, false);
        scaled.top = edge(box.top, scale_y, bound_h, false);
        scaled.right = edge(box.right, scale_x, bound_w, true);
        scaled.bottom = edge(box.bottom, scale_y, bound_h, true);
        scaled.score = box.score;
        if (scaled.left < scaled.right && scaled.top < scaled.bottom) {
            result.push_back(scaled);
        }
    }
    return result;
}

}  // namespace vision
