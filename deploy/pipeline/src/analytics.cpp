#include "vision/analytics.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace vision {
namespace {

struct RegionRule { std::string name; double x, y, width, height; };
struct LineRule { std::string name; double x1, y1, x2, y2; };

struct Event {
    std::string type, rule, direction, label;
    int class_id = -1;
    float score = 0;
    std::uint64_t track_id = 0, pts_us = 0;
    double x = 0, y = 0;
};

std::string decode_hex(const std::string& value) {
    if (value.empty() || value.size() % 2) throw std::runtime_error("invalid rule name hex");
    std::string result;
    result.reserve(value.size() / 2);
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < value.size(); i += 2) {
        const int high = nibble(value[i]), low = nibble(value[i + 1]);
        if (high < 0 || low < 0) throw std::runtime_error("invalid rule name hex");
        result.push_back(static_cast<char>((high << 4) | low));
    }
    if (result.empty() || result.size() > 192) throw std::runtime_error("invalid rule name");
    return result;
}

bool unit(double value) { return std::isfinite(value) && value >= 0 && value <= 1; }

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (unsigned char c : value) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20) out << "\\u" << std::hex << std::setw(4)
                              << std::setfill('0') << static_cast<int>(c) << std::dec;
            else out << static_cast<char>(c);
        }
    }
    return out.str();
}

std::string utc_now() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    std::tm value{};
    gmtime_r(&seconds, &value);
    std::ostringstream out;
    out << std::put_time(&value, "%Y-%m-%dT%H:%M:%S") << '.'
        << std::setw(3) << std::setfill('0') << millis << 'Z';
    return out.str();
}

std::string event_json(const Event& event) {
    std::ostringstream out;
    out << "{\"time\":\"" << utc_now() << "\",\"type\":\""
        << json_escape(event.type) << "\",\"rule\":\"" << json_escape(event.rule)
        << "\",\"direction\":\"" << json_escape(event.direction)
        << "\",\"label\":\"" << json_escape(event.label)
        << "\",\"class_id\":" << event.class_id
        << ",\"score\":" << std::fixed << std::setprecision(4) << event.score
        << ",\"track_id\":" << event.track_id << ",\"pts_us\":" << event.pts_us
        << ",\"center\":[" << std::setprecision(6) << event.x << ',' << event.y << "]}";
    return out.str();
}

} // namespace

struct AnalyticsEngine::Impl {
    struct Track {
        std::uint64_t id = 0;
        int class_id = -1;
        double x = 0, y = 0, width = 0, height = 0;
        int missed = 0;
        bool matched = false;
        std::vector<bool> regions;
        std::vector<double> line_sides;
        std::unordered_map<std::string, std::uint64_t> last_events;
    };

    Impl(const std::string& rules_path, std::string path,
         std::vector<std::string> names, int cooldown, int missed)
        : event_log(std::move(path)), class_names(std::move(names)),
          cooldown_us(static_cast<std::uint64_t>(cooldown) * 1000),
          max_missed(missed), queue(256) {
        if (cooldown < 0 || cooldown > 600000 || missed < 0 || missed > 30)
            throw std::invalid_argument("invalid analytics cooldown or missed-frame limit");
        if (!rules_path.empty()) load_rules(rules_path);
        if ((!regions.empty() || !lines.empty()) && event_log.empty())
            throw std::invalid_argument("analytics event log is required when rules are enabled");
        active.store(!regions.empty() || !lines.empty());
        if (active.load()) writer = std::thread([this] { writer_loop(); });
    }

    ~Impl() {
        // Events are sparse; give the writer a bounded shutdown window to
        // flush already accepted records without ever blocking the AI path.
        for (int attempt = 0; attempt < 50 && queue.size(); ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        running.store(false);
        queue.close();
        if (writer.joinable()) writer.join();
    }

    void load_rules(const std::string& path) {
        std::ifstream input(path);
        if (!input) throw std::runtime_error("cannot open analytics rules: " + path);
        std::string line;
        int line_number = 0;
        while (std::getline(input, line)) {
            ++line_number;
            if (line.empty() || line[0] == '#') continue;
            std::istringstream values(line);
            std::string type, encoded;
            values >> type >> encoded;
            if (type == "region") {
                RegionRule rule{};
                rule.name = decode_hex(encoded);
                values >> rule.x >> rule.y >> rule.width >> rule.height;
                if (!values || !unit(rule.x) || !unit(rule.y) || !unit(rule.width) ||
                    !unit(rule.height) || rule.width <= 0 || rule.height <= 0 ||
                    rule.x + rule.width > 1.000001 || rule.y + rule.height > 1.000001)
                    throw std::runtime_error("invalid analytics region at line " +
                                             std::to_string(line_number));
                regions.push_back(std::move(rule));
            } else if (type == "line") {
                LineRule rule{};
                rule.name = decode_hex(encoded);
                values >> rule.x1 >> rule.y1 >> rule.x2 >> rule.y2;
                if (!values || !unit(rule.x1) || !unit(rule.y1) || !unit(rule.x2) ||
                    !unit(rule.y2) || std::hypot(rule.x2 - rule.x1, rule.y2 - rule.y1) < .01)
                    throw std::runtime_error("invalid analytics line at line " +
                                             std::to_string(line_number));
                lines.push_back(std::move(rule));
            } else {
                throw std::runtime_error("unknown analytics rule at line " +
                                         std::to_string(line_number));
            }
            std::string extra;
            if (values >> extra) throw std::runtime_error("extra analytics rule fields at line " +
                                                           std::to_string(line_number));
        }
        if (regions.size() > 16 || lines.size() > 16)
            throw std::runtime_error("analytics supports at most 16 regions and 16 lines");
    }

    static double side(const LineRule& rule, double x, double y) {
        return (rule.x2 - rule.x1) * (y - rule.y1) -
               (rule.y2 - rule.y1) * (x - rule.x1);
    }

    bool can_emit(Track& track, const std::string& key, std::uint64_t pts) {
        const auto found = track.last_events.find(key);
        if (found != track.last_events.end() && pts >= found->second &&
            pts - found->second < cooldown_us) return false;
        track.last_events[key] = pts;
        return true;
    }

    void emit(Track& track, const Box& box, const std::string& type,
              const std::string& rule, const std::string& direction,
              std::uint64_t pts) {
        if (!can_emit(track, type + "\n" + rule, pts)) return;
        Event event;
        event.type = type; event.rule = rule; event.direction = direction;
        event.class_id = box.class_id; event.score = box.score;
        event.label = box.class_id >= 0 && static_cast<std::size_t>(box.class_id) < class_names.size()
                          ? class_names[static_cast<std::size_t>(box.class_id)]
                          : "class-" + std::to_string(box.class_id);
        event.track_id = track.id; event.pts_us = pts; event.x = track.x; event.y = track.y;
        ++generated;
        queue.push(std::move(event));
    }

    void update_rules(Track& track, const Box& box, bool fresh, std::uint64_t pts) {
        for (std::size_t i = 0; i < regions.size(); ++i) {
            const auto& rule = regions[i];
            const bool inside = track.x >= rule.x && track.x <= rule.x + rule.width &&
                                track.y >= rule.y && track.y <= rule.y + rule.height;
            if ((fresh && inside) || (!fresh && inside != track.regions[i]))
                emit(track, box, inside ? "region_enter" : "region_exit", rule.name, "", pts);
            track.regions[i] = inside;
        }
        constexpr double margin = .005;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const double current = side(lines[i], track.x, track.y);
            const double previous = track.line_sides[i];
            if (!fresh && std::abs(current) >= margin && std::abs(previous) >= margin &&
                ((current > 0) != (previous > 0))) {
                emit(track, box, "line_cross", lines[i].name,
                     previous < 0 ? "side_a_to_b" : "side_b_to_a", pts);
            }
            if (std::abs(current) >= margin || fresh) track.line_sides[i] = current;
        }
    }

    void process(const Detection& detection) {
        ++processed;
        if (!active.load() || detection.width <= 0 || detection.height <= 0) return;
        for (auto& track : tracks) { track.matched = false; ++track.missed; }
        for (const auto& box : detection.boxes) {
            if (box.class_id < 0 || box.left >= box.right || box.top >= box.bottom) continue;
            const double x = (box.left + box.right) * .5 / detection.width;
            const double y = (box.top + box.bottom) * .5 / detection.height;
            const double width = static_cast<double>(box.right - box.left) / detection.width;
            const double height = static_cast<double>(box.bottom - box.top) / detection.height;
            if (!unit(x) || !unit(y)) continue;
            Track* best = nullptr;
            double best_cost = std::numeric_limits<double>::max();
            for (auto& candidate : tracks) {
                if (candidate.matched || candidate.class_id != box.class_id ||
                    candidate.missed > max_missed + 1) continue;
                const double distance = std::hypot(candidate.x - x, candidate.y - y);
                const double size_delta = std::abs(candidate.width - width) +
                                          std::abs(candidate.height - height);
                const double cost = distance + size_delta * .25;
                if (distance <= .20 && cost < best_cost) { best = &candidate; best_cost = cost; }
            }
            bool fresh = false;
            if (!best) {
                Track value;
                value.id = next_track_id++; value.class_id = box.class_id;
                value.regions.assign(regions.size(), false);
                value.line_sides.assign(lines.size(), 0);
                tracks.push_back(std::move(value));
                best = &tracks.back(); fresh = true;
            }
            best->x = x; best->y = y; best->width = width; best->height = height;
            best->missed = 0; best->matched = true;
            update_rules(*best, box, fresh, detection.pts_us);
        }
        tracks.erase(std::remove_if(tracks.begin(), tracks.end(), [this](const Track& track) {
            return track.missed > max_missed;
        }), tracks.end());
        track_count.store(tracks.size());
    }

    void writer_loop() noexcept {
        Event event;
        while (running.load()) {
            if (!queue.pop(event, std::chrono::milliseconds(250))) continue;
            try {
                std::ifstream size_check(event_log, std::ios::binary | std::ios::ate);
                const auto size = size_check ? size_check.tellg() : std::streampos(0);
                size_check.close();
                if (size > std::streampos(8 * 1024 * 1024)) {
                    const std::string rotated = event_log + ".1";
                    std::remove(rotated.c_str());
                    std::rename(event_log.c_str(), rotated.c_str());
                }
                std::ofstream output(event_log, std::ios::app);
                if (!output) throw std::runtime_error("open event log failed");
                output << event_json(event) << '\n';
                output.flush();
                if (!output) throw std::runtime_error("write event log failed");
                ++written;
            } catch (...) { ++write_errors; }
        }
    }

    std::string event_log;
    std::vector<std::string> class_names;
    std::vector<RegionRule> regions;
    std::vector<LineRule> lines;
    std::vector<Track> tracks;
    std::uint64_t next_track_id = 1;
    std::uint64_t cooldown_us;
    int max_missed;
    LatestQueue<Event> queue;
    std::thread writer;
    std::atomic<bool> running{true}, active{false};
    std::atomic<std::uint64_t> processed{0}, generated{0}, written{0}, write_errors{0};
    std::atomic<std::size_t> track_count{0};
};

AnalyticsEngine::AnalyticsEngine(const std::string& rules_path, const std::string& event_log,
                                 std::vector<std::string> class_names,
                                 int cooldown_ms, int max_missed)
    : impl_(new Impl(rules_path, event_log, std::move(class_names),
                     cooldown_ms, max_missed)) {}
AnalyticsEngine::~AnalyticsEngine() = default;
bool AnalyticsEngine::enabled() const noexcept { return impl_->active.load(); }
void AnalyticsEngine::process(const Detection& detection) { impl_->process(detection); }
AnalyticsSnapshot AnalyticsEngine::snapshot() const {
    AnalyticsSnapshot result;
    result.processed = impl_->processed.load(); result.generated = impl_->generated.load();
    result.written = impl_->written.load(); result.dropped = impl_->queue.dropped();
    result.write_errors = impl_->write_errors.load(); result.tracks = impl_->track_count.load();
    result.regions = impl_->regions.size(); result.lines = impl_->lines.size();
    return result;
}

} // namespace vision
