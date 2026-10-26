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

The exact topology needs a coordinated BSP change, not only an application
change. A production implementation must provide one of the following:

- Rockchip-supported ISP bridge resize, including correct picture and gain
  plane scaling/stride propagation into RKISPP; or
- RKAiq support for routing the sensor's per-frame ISPP parameter stream to an
  offline `rkispp` instance whose input is the ISP mainpath DMA-BUF.

Either solution changes the kernel/RKAiq integration and requires a recovery
image, serial-console capture, image-quality validation across gain/exposure,
and regression testing before replacing the eMMC boot partition.

The experimental application code is retained only on its named feature
branch. It must not be merged into the release branch or deployed by the
service manager without the missing BSP support.
