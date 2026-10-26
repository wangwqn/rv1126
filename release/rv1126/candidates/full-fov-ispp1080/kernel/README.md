# RKISPP kernel backport candidate

This directory is for the boot image built by
`scripts/build-kernel-rkispp-backport.sh build`. The image is never flashed by
that script.

The patch is a clean, combined backport of these Rockchip `develop-4.19`
commits on top of the board SDK's RKISPP v1.6.2 source:

- `2a4f54dcf330` — fix monitor switch if power is retained;
- `75d5fa521cd8` — validate output buffers at frame start;
- `d49f43aafc6b` — disable scaler DMA when no output buffer exists;
- `cfe05e46c937` — fix monitor activation and stop handling;
- `193fab7512d3` — remove the old TNR IIR first-frame DMA shortcut.

Build or source preparation does not authorize writing the board's boot
partition. Before any board trial, preserve the stock boot image and use a
serial console or loader-mode recovery path.
