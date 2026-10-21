# RV1126 IMX415 真机验证

## 已固化的默认参数

```text
主流：1920x1080 输入，旋转 270 度后 1080x1920，H.264 Baseline，30 FPS，6 Mbps
子流：640x360 输入，旋转 270 度后 360x640，H.264 Baseline，30 FPS，600 kbps
AI：3 FPS，阈值 0.35
缓冲：VI/RGA 8，编码包队列 8
```

板载旧版 `librtsp` 在 TCP interleaved、主流 8 Mbps 时会出现 CSeq 和 H.264
损坏。2026-10-21 以子流固定 600 kbps、AI/OSD 开启、RTSP over TCP 同时解码两路：

- 5 Mbps 连续 60 秒：主、子流各 1800 帧，退出码 0，无解码或协议警告。
- 6 Mbps 连续 60 秒：主、子流各 1800 帧，退出码 0，无解码或协议警告。
- 6 Mbps 连续 120 秒：主、子流各 3600 帧，客户端约 30.13 FPS；服务端
  `pts_gap=0`、`queue_high_water=2/8`、`stale_drop=0`。

因此 6 Mbps 是当前开发板、BSP、旧 `librtsp` 和 Windows 热点链路组合中“已经
验证的稳定上限”，不是对所有网络环境的保证。8 Mbps 仍为已知失败点，中间的
7 Mbps 尚未测试；弱 Wi-Fi 环境可显式降为 5 Mbps 或 4 Mbps。

## RTP/UDP 传输策略

RTSP 控制连接仍使用 TCP 8554；H.264 媒体默认由客户端在 `SETUP` 中请求
`RTP/AVP/UDP`，以避免 TCP interleaved 的队头阻塞。板载 2020 版静态库已确认包含
UDP RTP/RTCP socket 和 `client_port/server_port` 协商实现，不需要更换服务端程序。

板端本机 FFmpeg 双路 UDP 30 秒测试均正常退出，服务端日志确认主、子流分别建立
UDP RTP/RTCP 端口。无额外接收缓冲时主流曾报告一次丢9个 RTP包；单主流使用
2 MiB socket buffer、512包重排队列和200 ms最大重排等待复测30秒无丢包。
这说明 UDP消除了发送阻塞，但网络拥塞时可能丢包；因此保留 TCP作为回退方式，
并保持主流6 Mbps，不因改用 UDP恢复到已知不稳定的8 Mbps。

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

早期真机隔离测试中，关闭 AI/OSD 后约 18 秒仍出现 17 个 `pts_gap`，把 VI/RGA
缓冲从 8 提到 16 也没有改善。根因是 TNR 关闭时机早于 VI/RKAIQ 完成初始化；改为
摄像头图启动后关闭 TNR，本次 6 Mbps 测试稳态 `pts_gap=0`。仍需用 30 分钟和
24 小时测试确认长期行为，不能用短测结果承诺绝对零丢帧。

## Ubuntu 经局域网 UDP 验证

```bash
ffprobe -v error -rtsp_transport udp \
  -select_streams v:0 \
  -show_entries stream=codec_name,profile,width,height,pix_fmt,r_frame_rate \
  -of default=noprint_wrappers=1 \
  rtsp://开发板当前IP:8554/live/main_stream

bash scripts/test_rtsp_udp.sh 开发板当前IP 1800
```

正式验收必须从另一台机器经实际 Wi-Fi/以太网直连开发板，脚本同时解码主、子流，
并确认无 `RTP: missed`、`corrupt decoded frame`、超时和进程退出。ADB forward 只能
转发 TCP，不能验证 RTP/UDP。24小时测试用于发现丢包、DMA-BUF、句柄或内存泄漏。

UDP 被网络策略阻断时可用 TCP 隔离诊断：

```bash
adb forward tcp:8554 tcp:8554
ffplay -rtsp_transport tcp rtsp://127.0.0.1:8554/live/main_stream
```

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
  -rtsp_transport udp -buffer_size 1048576 \
  -reorder_queue_size 128 -max_delay 100000 \
  -probesize 32 -analyzeduration 0 \
  rtsp://开发板当前IP:8554/live/main_stream
```

让摄像头拍摄毫秒计时器，同时在播放器中截图，对比“实物计时器”和“画面计时器”；
至少记录 100 次并统计 P50/P95/P99。当前尚未完成这项光学对拍，因此不能声称
端到端已经低于 200 ms。

