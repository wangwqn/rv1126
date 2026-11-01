#include "vision/dma_audit.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
} // namespace

int main() {
    vision::DmaAudit disabled;
    check(disabled.acquire(vision::DmaRoute::ai_rga, 7, 1024, 10) == 0,
          "disabled audit has no hot-path lease bookkeeping");

    vision::DmaAudit audit(1000);
    audit.enable(true);
    const auto first = audit.acquire(vision::DmaRoute::ai_rga, 7, 1024, 100);
    const auto second = audit.acquire(vision::DmaRoute::ai_rga, 7, 2048, 200);
    audit.timeout(vision::DmaRoute::main_venc, 300);
    audit.cpu_begin(first, vision::CpuAccessKind::bgr, 1024);
    audit.cpu_end(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));

    auto snapshot = audit.snapshot();
    check(snapshot.live_handles == 2 && snapshot.peak_handles == 2,
          "live MEDIA_BUFFER handles are counted");
    check(snapshot.live_fds == 1 && snapshot.peak_fds == 1,
          "duplicate borrowed fd is reference counted");
    check(snapshot.overdue_handles == 2,
          "long-held MEDIA_BUFFER leases are reported");
    check(snapshot.cpu_accesses == 1 && snapshot.cpu_active == 0 &&
              snapshot.cpu_kind[static_cast<std::size_t>(vision::CpuAccessKind::bgr)] == 1,
          "explicit CPU mapping and cache-sync scope are counted");
    check(snapshot.routes[static_cast<std::size_t>(vision::DmaRoute::main_venc)].timeouts == 1,
          "buffer acquisition timeouts are counted per route");

    audit.release(first, false);
    audit.release(second, true);
    snapshot = audit.snapshot();
    check(snapshot.live_handles == 0 && snapshot.live_fds == 0,
          "release retires handles and borrowed fds");
    check(snapshot.acquired == snapshot.released && snapshot.release_errors == 1,
          "release balance and SDK errors are visible");
    check(snapshot.routes[static_cast<std::size_t>(vision::DmaRoute::ai_rga)].hold.samples == 2,
          "hold-time percentiles include each released lease");

    const auto invalid = audit.acquire(vision::DmaRoute::other, -1, 0, 0);
    snapshot = audit.snapshot();
    check(snapshot.missing_fds == 1 && snapshot.required_dma_errors == 0,
          "non-DMA compressed MEDIA_BUFFER is counted without a false error");
    audit.release(invalid, false);

    const auto bad_image = audit.acquire(vision::DmaRoute::ai_rga, -1, 1024, 0);
    snapshot = audit.snapshot();
    check(snapshot.required_dma_errors == 1,
          "missing DMA fd on an RGA image is a zero-copy violation");
    audit.release(bad_image, false);

    std::cout << "DMA audit tests passed\n";
    return 0;
}
