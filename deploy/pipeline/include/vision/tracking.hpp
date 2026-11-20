#pragma once

#include "vision/core.hpp"

namespace vision {

// Metadata-only tracking: associate same-class NPU observations, estimate a
// bounded constant velocity, and project to the observed main-video PTS. No
// image mapping, optical flow, or extra NPU invocation is performed here.
// One OSD thread owns this object; the DetectionCache remains immutable.
class DetectionTracker {
public:
    explicit DetectionTracker(std::size_t capacity = 100) : capacity_(capacity) {
        if (!capacity) throw std::invalid_argument("tracker capacity must be positive");
    }

    void reset() {
        tracks_.clear();
        source_.reset();
        last_main_pts_ = 0;
        predicted_ = false;
    }

    bool predicted() const { return predicted_; }

    // A null/empty/expired selection is a hard clear, never a missed-frame
    // grace period. Predictions do not refresh the original detection's TTL.
    Detection project(const std::shared_ptr<const Detection>& selected,
                      std::uint64_t main_pts, std::uint64_t max_age_us,
                      std::chrono::milliseconds wall_ttl, bool enabled = true) {
        predicted_ = false;
        const auto now = std::chrono::steady_clock::now();
        if (!selected || selected->width <= 0 || selected->height <= 0 ||
            selected->pts_us > main_pts || wall_ttl.count() < 0 ||
            main_pts - selected->pts_us > max_age_us ||
            selected->completed_at > now || now - selected->completed_at > wall_ttl) {
            reset();
            return {};
        }
        if ((last_main_pts_ && main_pts < last_main_pts_) ||
            (source_ && (selected->width != source_->width ||
                         selected->height != source_->height ||
                         selected->pts_us <= source_->pts_us) && selected != source_)) {
            reset();
        }
        if (selected != source_) {
            ingest(*selected, max_age_us);
            source_ = selected;
        }
        last_main_pts_ = main_pts;
        Detection output;
        output.pts_us = main_pts;
        output.width = selected->width;
        output.height = selected->height;
        output.completed_at = selected->completed_at;
        const double dt = enabled ? (main_pts - selected->pts_us) / 1000000.0 : 0.0;
        for (const auto& track : tracks_) {
            output.boxes.push_back(project_box(track, dt, output.width, output.height));
            predicted_ = predicted_ || (dt > 0.0 && (track.vx != 0.0 || track.vy != 0.0));
        }
        return output;
    }

private:
    struct Track {
        Box box;
        double vx = 0.0, vy = 0.0;  // pixels/second in rotated AI coordinates
        bool velocity = false;
    };
    static double cx(const Box& b) { return (double(b.left) + b.right) * 0.5; }
    static double cy(const Box& b) { return (double(b.top) + b.bottom) * 0.5; }
    static Box project_box(const Track& t, double dt, int width, int height) {
        Box b = t.box;
        const int w = b.right - b.left, h = b.bottom - b.top;
        // Shift the whole box back inside the image without shrinking it.
        b.left = static_cast<int>(std::lround(std::max(0.0, std::min(
            double(width - w), b.left + t.vx * dt))));
        b.top = static_cast<int>(std::lround(std::max(0.0, std::min(
            double(height - h), b.top + t.vy * dt))));
        b.right = b.left + w;
        b.bottom = b.top + h;
        return b;
    }
    static double affinity(const Box& previous, const Box& next) {
        if (previous.class_id != next.class_id) return -1.0;
        const double pw = previous.right - previous.left, ph = previous.bottom - previous.top;
        const double nw = next.right - next.left, nh = next.bottom - next.top;
        if (nw / pw < 0.5 || nw / pw > 2.0 || nh / ph < 0.5 || nh / ph > 2.0)
            return -1.0;
        const double iw = std::max(0, std::min(previous.right, next.right) -
                                      std::max(previous.left, next.left));
        const double ih = std::max(0, std::min(previous.bottom, next.bottom) -
                                      std::max(previous.top, next.top));
        const double intersection = iw * ih;
        const double iou = intersection / (pw * ph + nw * nh - intersection);
        const double dx = (cx(previous) - cx(next)) / std::max(pw, nw);
        const double dy = (cy(previous) - cy(next)) / std::max(ph, nh);
        const double distance = dx * dx + dy * dy;
        // Nearby non-overlapping observations may still be the same moving
        // object; reject large jumps and prefer IoU where available.
        if (iou < 0.1 && distance > 1.0) return -1.0;
        return iou + 0.25 / (1.0 + distance);
    }
    void ingest(const Detection& detection, std::uint64_t max_age_us) {
        std::vector<Track> next;
        std::vector<std::size_t> order;
        for (std::size_t i = 0; i < detection.boxes.size(); ++i)
            if (std::isfinite(detection.boxes[i].score)) order.push_back(i);
        std::stable_sort(order.begin(), order.end(), [&detection](std::size_t a, std::size_t b) {
            return detection.boxes[a].score > detection.boxes[b].score;
        });
        std::vector<bool> used(tracks_.size(), false);
        const bool consecutive = source_ && detection.pts_us > source_->pts_us &&
            detection.pts_us - source_->pts_us <= max_age_us;
        const double dt = consecutive ? (detection.pts_us - source_->pts_us) / 1000000.0 : 0.0;
        for (const auto index : order) {
            if (next.size() == capacity_) break;
            Box box = detection.boxes[index];
            if (!std::isfinite(box.score)) continue;
            box.left = std::max(0, std::min(detection.width, box.left));
            box.right = std::max(0, std::min(detection.width, box.right));
            box.top = std::max(0, std::min(detection.height, box.top));
            box.bottom = std::max(0, std::min(detection.height, box.bottom));
            if (box.right <= box.left || box.bottom <= box.top) continue;
            Track track;
            track.box = box;
            std::size_t best = tracks_.size();
            double score = -1.0;
            if (dt > 0.0) {
                for (std::size_t i = 0; i < tracks_.size(); ++i) {
                    if (used[i]) continue;
                    const auto projected = project_box(tracks_[i], dt, detection.width,
                                                       detection.height);
                    const double candidate = affinity(projected, box);
                    if (candidate > score) { score = candidate; best = i; }
                }
            }
            if (best < tracks_.size()) {
                used[best] = true;
                const auto& old = tracks_[best];
                const double vx = (cx(box) - cx(old.box)) / dt;
                const double vy = (cy(box) - cy(old.box)) / dt;
                // Reject noisy speed spikes; one-frame displacement remains
                // bounded even if a detector changes ordering or box size.
                const double bound_x = detection.width * 2.0;
                const double bound_y = detection.height * 2.0;
                track.vx = std::max(-bound_x, std::min(bound_x,
                    old.velocity ? 0.5 * old.vx + 0.5 * vx : vx));
                track.vy = std::max(-bound_y, std::min(bound_y,
                    old.velocity ? 0.5 * old.vy + 0.5 * vy : vy));
                track.velocity = true;
            }
            next.push_back(track);
        }
        // Unmatched tracks are retired immediately. Never keep a vanished
        // object alive behind an explicit empty NPU result.
        tracks_.swap(next);
    }
    const std::size_t capacity_;
    std::shared_ptr<const Detection> source_;
    std::vector<Track> tracks_;
    std::uint64_t last_main_pts_ = 0;
    bool predicted_ = false;
};

inline bool same_overlay_boxes(const std::vector<Box>& a, const std::vector<Box>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [](const Box& x, const Box& y) {
            return x.left == y.left && x.top == y.top && x.right == y.right &&
                   x.bottom == y.bottom && x.score == y.score && x.class_id == y.class_id;
        });
}
} // namespace vision
