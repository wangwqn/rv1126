# IMX415 全视场 1080P30 候选链路

这是隔离部署的硬件验证候选，不覆盖 `/root/rv1126_objects` 正式版本。

目标链路：

```text
IMX415 3864x2192 RAW10（有效 3840x2160）
  -> RKISP0 mainpath/MRSZ 1920x1080 NV12
  -> DMA-BUF -> rkispp1 DDR input
  -> TNR 2to1 1920x1080
  -> scale0 1920x1080 + scale1 640x360
  -> H.264 RTSP 30 FPS
```

`librkaiq.so` 只由候选启动脚本通过 `LD_LIBRARY_PATH` 加载，不替换开发板
`/usr/lib/librkaiq.so`。程序还会检查补丁导出标记，误加载系统库时直接退出。

部署和首次无干扰测试：

```bash
bash scripts/push_full_fov_candidate.sh
adb shell '/root/rv1126_objects/rv1126-object stop'
adb shell '/root/rv1126_full_fov_candidate/rv1126-full-fov start --no-ai --no-osd --no-audio --sensor-mode 4k-ispp-1080 --fps 30'
adb shell '/root/rv1126_full_fov_candidate/rv1126-full-fov health'
```

确认基础视频链路稳定后，停止并用默认参数启动完整 AI、OSD、音频功能：

```bash
adb shell '/root/rv1126_full_fov_candidate/rv1126-full-fov restart'
```

主/子流分别为：

```text
rtsp://开发板IP:8554/live/main_stream
rtsp://开发板IP:8554/live/sub_stream
```

如果开发板失去响应，断电重启即可恢复正常媒体拓扑；候选方案没有写 boot 分区，
也没有替换任何系统库。
