# DMA-BUF zero-copy audit

## Runtime switch

Start the pipeline with `--dma-audit`. The switch also enables the existing
rolling latency statistics. Every five seconds the process emits three extra
lines:

```text
dma_buf handles(...) fd(...) cpu(...) process_dma(...) kernel_dma(...)
dma_wait_ms p50/p95/p99/max ai_rga2(...) main_venc0(...) sub_venc1(...)
dma_graph main=... sub=... ai=... osd=... pool_config(...)
```

`handles` counts application-owned `MEDIA_BUFFER` leases from
`RK_MPI_SYS_GetMediaBuffer` until the matching RAII destructor calls
`RK_MPI_MB_ReleaseBuffer`. `live == 0` is not expected while the pipeline is
busy, but `acquired - released` must stay bounded. `overdue` reports a lease
held for at least one second. VENC compressed packets are CPU-visible and may
increase `missing`; only `required_error` means a required RGA image lost its
DMA backing. Any `required_error`, `release_error`, `sync_error` or
`WARNING=buffer_lifecycle` is a failure that needs investigation.

`process_dma` scans `/proc/self/fdinfo` for `exp_name`; `kernel_dma` reads
`/sys/kernel/debug/dma_buf/bufinfo`. These include SDK-owned hardware pool
buffers that never become application `MEDIA_BUFFER` leases. The old RKMedia
SDK does not expose exact per-channel pool occupancy, so the output deliberately
says `exact_occupancy=not_exposed_by_RKMedia` instead of inventing a percentage.

## What is and is not zero-copy

- Main: VI NV12 DMA-BUF -> bound RGA0 NV12 -> bound VENC0. No application CPU
  mapping or full-frame copy.
- Sub: VI NV12 DMA-BUF -> bound RGA1 NV12 -> bound VENC1. No application CPU
  mapping or full-frame copy.
- AI: VI NV12 DMA-BUF -> RGA2 packed BGR DMA-BUF. RockX/RKNN uses a CPU-visible
  BGR pointer, explicitly bracketed by Begin/EndCPUAccess. This is a mapping,
  not an application NV12 memcpy; the NPU runtime may perform its own internal
  import/copy, which this first-generation SDK does not expose.
- OSD: CPU renders small BGRA box/label bitmaps, then the RGA region API blends
  them into the main NV12 hardware path. The 1080p frame itself is not mapped.
- RTSP: VENC output is already compressed H.264 and this BSP may expose it
  without a DMA FD. It must be CPU-read to split NAL units and packetize RTP;
  this is not a raw NV12 frame copy.

Therefore the key runtime invariant is `cpu(nv12=0)`. BGR and encoded mappings
are intentional and must have `sync_error=0`.

## Stride and RGA timing

RGA channels are configured with explicit horizontal/vertical stride equal to
their hardware branch dimensions. The AI path validates returned image format,
width, height, horizontal stride, vertical stride and backing size before
creating its `cv::Mat` view.

`dma_wait_ms` is the blocking wait to obtain an RGA/VENC output and the time an
application holds that output. It is not a fabricated RGA execution time. The
bound main/sub RGA channels do not expose per-operation start/completion events
in this RKMedia BSP. `latency_ms osd(update=...)` measures the RGA region update
call, while `ai(rga_age=...)` measures the age of the produced AI frame.

## Long-run collection

From Ubuntu, with ADB connected:

```bash
bash scripts/capture_dmabuf_audit.sh 1800
```

The output directory contains one-second kernel/process samples, application
DMA audit lines and a start/end drift summary. For a stable run, process FD and
RSS deltas should remain bounded and the application log must not contain
`WARNING=buffer_lifecycle`.
