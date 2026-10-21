# RV1126 IMX415 真机验证

## 已固化的默认参数

```text
主流：1920x1080 输入，旋转 270 度后 1080x1920，H.264 Baseline，30 FPS，4 Mbps
子流：640x360 输入，旋转 270 度后 360x640，H.264 Baseline，30 FPS，600 kbps
AI：3 FPS，阈值 0.35
缓冲：VI/RGA 8，编码包队列 8
```

板载旧版 `librtsp` 在 TCP interleaved、主流 8 Mbps 时会出现 CSeq 和 H.264
损坏；降为 4 Mbps 后，主、子双路并发接收 20 秒均为 585 帧且无协议或码流错误。
585/20 秒也说明客户端实际约为 29.x FPS，而不是绝对无缺口的 30.000 FPS；因此
当前只达到了稳定的“名义 30 FPS”，尚未达到严格的“零丢帧”。

## 启动

```bash
adb shell
cd /root/rv1126_objects
./rkmedia_object_detection_rtsp --rotation 270 --threshold 0.35
```

其余参数使用上方默认值。调试时建议前台运行，以便每 5 秒查看：

```text
main/sub venc、pts_gap、pts_reset、AI sampled/completed/stale_drop、
OSD boxes/truncated、RTSP queue_high_water/shutdown_abandoned
```

`pts_reset` 是 ISP 启动阶段超过 1 秒的时钟跳变，不计为成百上千个丢帧；
稳态 `pts_gap` 才表示按 30 FPS 时间基准推算的缺帧数。

真机隔离测试中，关闭 AI/OSD 后约 18 秒仍出现 17 个 `pts_gap`；把 VI/RGA
缓冲从 8 提到 16 也没有改善。由此可排除 AI 背压和缓冲池不足，当前缺口位于
IMX415→RKAIQ/RKMedia 输入节拍或旧 BSP 驱动侧。修复需要升级/修改相机 BSP 并重新
测量，而不是在应用层伪造重复帧；重复帧只能让计数变成 30，不能满足“零丢帧”。

## Ubuntu 经 USB ADB 验证

```bash
adb forward tcp:8554 tcp:8554

ffprobe -v error -rtsp_transport tcp \
  -select_streams v:0 \
  -show_entries stream=codec_name,profile,width,height,pix_fmt,r_frame_rate \
  -of default=noprint_wrappers=1 \
  rtsp://127.0.0.1:8554/live/main_stream

ffmpeg -hide_banner -loglevel warning -stats -rtsp_transport tcp \
  -i rtsp://127.0.0.1:8554/live/main_stream -map 0:v:0 -c copy \
  -t 1800 -f null -
```

子流把 URL 改为 `/live/sub_stream`。正式验收应让两条命令并发运行至少 30 分钟，
并确认无 `CSeq`、`corrupt decoded frame`、超时和进程退出。24 小时测试用于发现
DMA-BUF、句柄或普通内存泄漏。

## CPU 与内存

BusyBox `top` 默认以单核为 100%，四核 RV1126 上显示 100% 等于整机容量约 25%。
当前双路客户端与 AI 3 FPS 同时运行时，按 `/proc` 总时钟滴答测得应用为 25.23%，
RSS/HWM 约 119948 kB。该口径只统计应用进程；当次整机 busy 为 47.85%，其中还含
内核、ISP、USB ADB 和网络。验收时必须明确采用“应用进程”还是“整机”口径。

## DMA-BUF 与 OSD

```bash
pid=$(pidof rkmedia_object_detection_rtsp)
ls -l /proc/$pid/fd | grep dmabuf
```

主、子视频链路为 `VI DMA-BUF -> RGA NV12 -> VENC`，应用不做整帧 memcpy。
AI 独占低分辨率 RGA BGR 分支；中文标签只生成小型 ARGB 位图，框由 RGA region
合成。抓帧已经确认颜色、270 度旋转、绿色框和中文名称均正常。

## 低时延验收

低缓存播放器只能减少客户端额外缓存，不能代替端到端测量：

```bash
ffplay -fflags nobuffer -flags low_delay -framedrop \
  -rtsp_transport tcp -probesize 32 -analyzeduration 0 \
  rtsp://127.0.0.1:8554/live/main_stream
```

让摄像头拍摄毫秒计时器，同时在播放器中截图，对比“实物计时器”和“画面计时器”；
至少记录 100 次并统计 P50/P95/P99。当前尚未完成这项光学对拍，因此不能声称
端到端已经低于 200 ms。

