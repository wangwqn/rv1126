#pragma once

#include "vision/core.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace vision {

enum class DmaRoute : std::size_t {
    ai_rga = 0,
    main_venc = 1,
    sub_venc = 2,
    other = 3,
    count = 4,
};

enum class CpuAccessKind : std::size_t {
    nv12 = 0,
    bgr = 1,
    encoded = 2,
    count = 3,
};

const char* dma_route_name(DmaRoute route) noexcept;

struct DmaRouteSnapshot {
    std::uint64_t acquired = 0;
    std::uint64_t released = 0;
    std::uint64_t live = 0;
    std::uint64_t peak = 0;
    std::uint64_t timeouts = 0;
    LatencySnapshot get_wait;
    LatencySnapshot hold;
};

struct DmaKernelSnapshot {
    bool global_available = false;
    std::uint64_t global_objects = 0;
    std::uint64_t global_bytes = 0;
    bool process_available = false;
    std::uint64_t process_fds = 0;
    std::uint64_t process_bytes = 0;
};

struct DmaAuditSnapshot {
    bool enabled = false;
    std::uint64_t acquired = 0;
    std::uint64_t released = 0;
    std::uint64_t live_handles = 0;
    std::uint64_t peak_handles = 0;
    std::uint64_t live_fds = 0;
    std::uint64_t peak_fds = 0;
    std::uint64_t invalid_fds = 0;
    std::uint64_t release_errors = 0;
    std::uint64_t overdue_handles = 0;
    std::uint64_t max_live_hold_us = 0;
    std::uint64_t cpu_accesses = 0;
    std::uint64_t cpu_active = 0;
    std::uint64_t cpu_peak = 0;
    std::uint64_t cpu_bytes = 0;
    std::uint64_t cpu_sync_errors = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(CpuAccessKind::count)>
        cpu_kind{{0, 0, 0}};
    std::array<DmaRouteSnapshot, static_cast<std::size_t>(DmaRoute::count)> routes;
    DmaKernelSnapshot kernel;
};

// Tracks only application-owned MEDIA_BUFFER leases. RKMedia owns additional
// pool buffers and does not expose exact per-pool occupancy in this BSP. The
// kernel/process snapshot makes that limitation visible instead of guessing.
class DmaAudit {
public:
    explicit DmaAudit(std::uint64_t overdue_us = 1000000);
    DmaAudit(const DmaAudit&) = delete;
    DmaAudit& operator=(const DmaAudit&) = delete;

    void enable(bool value) noexcept;
    bool enabled() const noexcept { return enabled_.load(); }
    std::uint64_t acquire(DmaRoute route, int fd, std::size_t bytes,
                          std::uint64_t wait_us);
    void timeout(DmaRoute route, std::uint64_t wait_us);
    void release(std::uint64_t lease, bool sdk_error) noexcept;
    void cpu_begin(std::uint64_t lease, CpuAccessKind kind,
                   std::size_t bytes) noexcept;
    void cpu_end(bool sync_error) noexcept;
    DmaAuditSnapshot snapshot() const;

private:
    struct Lease {
        DmaRoute route = DmaRoute::other;
        int fd = -1;
        std::size_t bytes = 0;
        std::chrono::steady_clock::time_point acquired_at;
    };
    struct RouteState {
        std::uint64_t acquired = 0;
        std::uint64_t released = 0;
        std::uint64_t live = 0;
        std::uint64_t peak = 0;
        std::uint64_t timeouts = 0;
        LatencyWindow wait;
        LatencyWindow hold;
    };

    static DmaKernelSnapshot read_kernel_snapshot();
    const std::uint64_t overdue_us_;
    std::atomic<bool> enabled_{false};
    mutable std::mutex mutex_;
    std::uint64_t next_lease_ = 1;
    std::uint64_t acquired_ = 0, released_ = 0, peak_handles_ = 0;
    std::uint64_t peak_fds_ = 0, invalid_fds_ = 0, release_errors_ = 0;
    std::uint64_t cpu_accesses_ = 0, cpu_active_ = 0, cpu_peak_ = 0;
    std::uint64_t cpu_bytes_ = 0, cpu_sync_errors_ = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(CpuAccessKind::count)>
        cpu_kind_{{0, 0, 0}};
    std::array<RouteState, static_cast<std::size_t>(DmaRoute::count)> routes_;
    std::unordered_map<std::uint64_t, Lease> leases_;
    std::unordered_map<int, std::uint64_t> fd_refs_;
};

DmaAudit& dma_audit();

} // namespace vision
