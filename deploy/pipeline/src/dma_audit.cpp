#include "vision/dma_audit.hpp"
#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>

namespace vision {
namespace {
std::uint64_t elapsed_us(std::chrono::steady_clock::time_point start,
                         std::chrono::steady_clock::time_point end) {
    if (end <= start) return 0;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

bool numeric_name(const char* value) {
    if (!value || !*value) return false;
    for (const char* p = value; *p; ++p)
        if (!std::isdigit(static_cast<unsigned char>(*p))) return false;
    return true;
}
} // namespace

const char* dma_route_name(DmaRoute route) noexcept {
    switch (route) {
    case DmaRoute::ai_rga: return "ai_rga2";
    case DmaRoute::main_venc: return "main_venc0";
    case DmaRoute::sub_venc: return "sub_venc1";
    case DmaRoute::other: return "other";
    default: return "invalid";
    }
}

DmaAudit::DmaAudit(std::uint64_t overdue_us) : overdue_us_(overdue_us) {}

void DmaAudit::enable(bool value) noexcept { enabled_.store(value); }

std::uint64_t DmaAudit::acquire(DmaRoute route, int fd, std::size_t bytes,
                                std::uint64_t wait_us) {
    if (!enabled()) return 0;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto index = static_cast<std::size_t>(route);
    RouteState& state = routes_[index];
    state.wait.observe_us(wait_us);
    ++state.acquired;
    ++state.live;
    state.peak = std::max(state.peak, state.live);
    ++acquired_;
    peak_handles_ = std::max<std::uint64_t>(peak_handles_, leases_.size() + 1);
    if (fd < 0) {
        ++invalid_fds_;
    } else {
        ++fd_refs_[fd];
        peak_fds_ = std::max<std::uint64_t>(peak_fds_, fd_refs_.size());
    }
    const std::uint64_t lease = next_lease_++;
    leases_.emplace(lease, Lease{route, fd, bytes, now});
    return lease;
}

void DmaAudit::timeout(DmaRoute route, std::uint64_t wait_us) {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    RouteState& state = routes_[static_cast<std::size_t>(route)];
    state.wait.observe_us(wait_us);
    ++state.timeouts;
}

void DmaAudit::release(std::uint64_t lease, bool sdk_error) noexcept {
    if (!lease) return;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = leases_.find(lease);
    if (found == leases_.end()) return;
    const Lease value = found->second;
    RouteState& state = routes_[static_cast<std::size_t>(value.route)];
    state.hold.observe_us(elapsed_us(value.acquired_at, now));
    ++state.released;
    if (state.live) --state.live;
    ++released_;
    if (sdk_error) ++release_errors_;
    if (value.fd >= 0) {
        const auto fd = fd_refs_.find(value.fd);
        if (fd != fd_refs_.end() && --fd->second == 0) fd_refs_.erase(fd);
    }
    leases_.erase(found);
}

void DmaAudit::cpu_begin(std::uint64_t lease, CpuAccessKind kind,
                         std::size_t bytes) noexcept {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    ++cpu_accesses_;
    ++cpu_active_;
    cpu_peak_ = std::max(cpu_peak_, cpu_active_);
    cpu_bytes_ += bytes;
    ++cpu_kind_[static_cast<std::size_t>(kind)];
    if (lease && leases_.find(lease) == leases_.end()) ++cpu_sync_errors_;
}

void DmaAudit::cpu_end(bool sync_error) noexcept {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (cpu_active_) --cpu_active_;
    else ++cpu_sync_errors_;
    if (sync_error) ++cpu_sync_errors_;
}

DmaKernelSnapshot DmaAudit::read_kernel_snapshot() {
    DmaKernelSnapshot result;
    std::ifstream global("/sys/kernel/debug/dma_buf/bufinfo");
    std::string line;
    if (global) {
        result.global_available = true;
        while (std::getline(global, line)) {
            if (line.empty() || !std::isdigit(static_cast<unsigned char>(line[0])))
                continue;
            std::istringstream input(line);
            std::string size, flags, mode, count, exporter, inode;
            if (input >> size >> flags >> mode >> count >> exporter >> inode) {
                try {
                    result.global_bytes += std::stoull(size, nullptr, 10);
                    ++result.global_objects;
                } catch (const std::exception&) {
                }
            }
        }
    }

    DIR* directory = ::opendir("/proc/self/fdinfo");
    if (!directory) return result;
    result.process_available = true;
    while (const dirent* entry = ::readdir(directory)) {
        if (!numeric_name(entry->d_name)) continue;
        std::ifstream info(std::string("/proc/self/fdinfo/") + entry->d_name);
        std::uint64_t bytes = 0;
        bool dma = false;
        while (std::getline(info, line)) {
            if (line.compare(0, 5, "size:") == 0) {
                std::istringstream value(line.substr(5));
                value >> bytes;
            } else if (line.compare(0, 9, "exp_name:") == 0) {
                dma = true;
            }
        }
        if (dma) {
            ++result.process_fds;
            result.process_bytes += bytes;
        }
    }
    ::closedir(directory);
    return result;
}

DmaAuditSnapshot DmaAudit::snapshot() const {
    DmaAuditSnapshot result;
    result.enabled = enabled();
    if (!result.enabled) return result;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result.acquired = acquired_;
        result.released = released_;
        result.live_handles = leases_.size();
        result.peak_handles = peak_handles_;
        result.live_fds = fd_refs_.size();
        result.peak_fds = peak_fds_;
        result.invalid_fds = invalid_fds_;
        result.release_errors = release_errors_;
        result.cpu_accesses = cpu_accesses_;
        result.cpu_active = cpu_active_;
        result.cpu_peak = cpu_peak_;
        result.cpu_bytes = cpu_bytes_;
        result.cpu_sync_errors = cpu_sync_errors_;
        result.cpu_kind = cpu_kind_;
        for (const auto& item : leases_) {
            const std::uint64_t age = elapsed_us(item.second.acquired_at, now);
            result.max_live_hold_us = std::max(result.max_live_hold_us, age);
            if (age >= overdue_us_) ++result.overdue_handles;
        }
        for (std::size_t index = 0; index < routes_.size(); ++index) {
            const RouteState& source = routes_[index];
            DmaRouteSnapshot& target = result.routes[index];
            target.acquired = source.acquired;
            target.released = source.released;
            target.live = source.live;
            target.peak = source.peak;
            target.timeouts = source.timeouts;
        }
    }
    // Percentile sorting is intentionally outside the lease-map mutex. Each
    // LatencyWindow has its own short append lock, so diagnostics cannot stall
    // all MEDIA_BUFFER acquire/release operations while sorting samples.
    for (std::size_t index = 0; index < routes_.size(); ++index) {
        result.routes[index].get_wait = routes_[index].wait.snapshot();
        result.routes[index].hold = routes_[index].hold.snapshot();
    }
    result.kernel = read_kernel_snapshot();
    return result;
}

DmaAudit& dma_audit() {
    static DmaAudit instance;
    return instance;
}

} // namespace vision
