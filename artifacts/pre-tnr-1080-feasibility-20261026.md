# IMX415 full-FOV pre-TNR 1080p feasibility report

Date: 2026-10-26

Experimental branch: `feature/full-fov-pre-tnr-1080`

## Requested topology

```text
IMX415 3864x2192 RAW10 (active crop 3840x2160)
  -> RKISP full-FOV resize to 1920x1080
  -> YUV/FBC
  -> RKISPP TNR 2-to-1 at 1920x1080
  -> scale0 1920x1080 + scale1 640x360
  -> H.264 RTSP at 30 FPS
```

## Verified hardware and driver state

- The IMX415 driver provides 3864x2192 RAW10 at 30 FPS and reports an active
  3840x2160 crop at `(12,16)`.
- The board runs Rockchip Linux 4.19.111 with RKISP/RKISPP driver v01.06.02.
- RKISP and RKISPP are built into the kernel, not loadable modules.
- The normal direct bridge produces FBC420 3840x2160. RKISPP0 runs TNR 2-to-1
  on that 4K input and then scale0/scale1 produce 1920x1080 and 640x360.
- ISP20's bridge has no userspace-selectable output resize. Its selection is a
  dual crop, not a full-frame scale. Rockchip's current public develop-4.19
  bridge driver has the same limitation.

## Offline DMA-BUF experiments

The BSP exposes `rkispp_input_image`, and RKMedia VP can pass a VI DMA-BUF to
it without a CPU copy.

1. `rkisp_mainpath` successfully negotiates 1920x1080 NV12 from the full 4K
   ISP input.
2. Rockchip's board-supplied `rkmedia_vi_vp_vo_test` was run against the
   independent `/dev/media4` (`rkispp1`, input `/dev/video37`). It produced
   1920x1080 and 640x360 frames at approximately 33.3 ms intervals for 12
   seconds with `ErrCnt:0`. This validates the VP/DMA-BUF mechanism.
3. `/proc/rkispp1` showed `TNR OFF`, `NR OFF`, and `SHARP OFF`. RKAiq is bound
   to the sensor's `rkispp0` and does not submit per-frame parameter buffers to
   the independent offline instance.
4. Reconfiguring the sensor-associated `rkispp0` itself for DDR input, both
   before and after RKAiq initialization, wedges the 4.19.111 media driver on
   the first streamed frame and makes ADB unresponsive until a power cycle.
   This mode must not be shipped.

## Why a fixed TNR register setup is not an acceptable workaround

TNR/NR/sharpening are configured through the metadata-output node using
`struct rkispp_params_cfg`. RKAiq updates the coefficients and enable masks in
step with exposure and sensor gain. Enabling only the TNR bit, or sending one
hard-coded parameter block, loses that synchronization and can produce severe
ghosting, noise pumping, or invalid gain-buffer use.

## Production-safe choices with the current BSP

1. Preserve full FOV and RKAiq quality processing:
   `4K RAW10 -> ISP/FBC 4K -> ISPP TNR 4K -> scale0 1080p + scale1 360p`.
   This is the validated v1.8.7 path, but the observed rate is about 29.x FPS.
2. Preserve native 30 FPS and 1080p TNR load:
   use the IMX415 1080p sensor mode. This changes the sensor FOV.
3. Use ISP full-FOV 1080p plus independent offline ISPP:
   this reaches 30 FPS but has no valid RKAiq TNR/NR/sharp parameters and must
   not be presented as the requested TNR pipeline.

## Work required for the exact topology

The exact topology needs coordinated RKAiq/application handling, not only a
different pair of VI node names. A production implementation must provide one
of the following:

- Rockchip-supported ISP bridge resize, including correct picture and gain
  plane scaling/stride propagation into RKISPP; or
- RKAiq support for routing the sensor's per-frame ISPP parameter stream to an
  offline `rkispp` instance whose input is the ISP mainpath DMA-BUF.

The kernel route would require a recovery image and serial-console capture
before replacing the eMMC boot partition. The RKAiq route can be validated as
an application-local shared library, but still requires image-quality checks
across gain/exposure and full regression testing.

## Isolated RKAiq candidate prepared

The second option is now implemented as a reversible userspace candidate:

- `RKAIQ_ISPP_TARGET=1` routes sensor ISP0's dynamic ISPP parameters and stats
  to independent `rkispp1`.
- `RKAIQ_ISPP_INPUT_SIZE=1920x1080` prevents RKAiq from replacing the offline
  sink size with the 4K ISP source-pad size.
- the application resolves input, scale0, scale1 and subdevice nodes from the
  `rkispp1` media model, rather than taking the first globally duplicated
  entity name;
- a required exported marker makes the program refuse this mode if the stock
  RKAiq library was accidentally loaded;
- the custom library is loaded only from
  `/root/rv1126_full_fov_candidate` with `LD_LIBRARY_PATH`; `/usr/lib` and the
  boot partition are unchanged.

The ARM application and RKAiq library compile successfully, portable host
tests pass, and the release bundle passes SHA-256 verification.

## Board trials and startup-order correction

The first board trial remained responsive but produced no encoder frames. It
proved that setting `RKAIQ_ISPP_TARGET` from `main()` is too late: RKAiq maps
the media graph in its ELF constructor, before application code runs. The r2
candidate therefore injects both override variables in the launcher before
`exec()` and exports a query function; the application refuses to touch the
media graph unless the library confirms target 1 was actually applied.

The query initially rejected startup and exposed a second detail: this board's
IMX415 reaches ISP0 through CIF, while the first override covered only sensors
directly attached to ISP. The r3 library covers both enumeration branches and
confirmed the target, but starting it wedged the old driver and made ADB
`offline`. Source inspection shows that this driver does implement TNR 2to1
for `INP_DDR`; the remaining difference is ordering. The old application
started RKAiq's ISPP parameter/statistics streams before RKMedia had opened the
DDR input and output nodes.

The r4 candidate therefore prepares RKAiq first, opens/configures
`/dev/video37`, `/dev/video39`, and `/dev/video40`, starts RKAiq only after
those queues exist, and binds ISP mainpath to VP last. It compiled locally and
passed the host tests and package hashes. The board trial still made ADB go
`offline`, so application ordering alone is not sufficient. The candidate
remains isolated from the normal service and must not be promoted to `main`.

## RKISPP v1.6.2 driver finding

The board SDK is immediately before a sequence of Rockchip's own
`develop-4.19` RKISPP fixes. Five commits apply cleanly to this exact source:

- monitor state is reinitialized and checked before recovery work;
- scale output buffers are checked at frame start;
- scaler DMA is disabled when no output buffer is available;
- the obsolete TNR IIR first-frame DMA shortcut is removed.

These changes directly cover the failure window seen when independent DDR
input, TNR and two scaler outputs start together. They are packaged as
`patches/kernel/0001-rkispp-v1.7-buffer-and-tnr-first-frame-fixes.patch`.
`scripts/build-kernel-rkispp-backport.sh` can report, apply, reverse or build
the candidate. It deliberately has no flash command. A serial console or a
verified loader-mode recovery path is required before testing the boot image.
