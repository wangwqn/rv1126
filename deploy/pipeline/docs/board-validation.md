# RV1126 IMX415 真机验证

## 已固化的默认参数

```text
主流：1920x1080 输入，旋转 270 度后 1080x1920，H.264 Baseline，30 FPS，4 Mbps
子流：640x360 输入，旋转 270 度后 360x640，H.264 Baseline，30 FPS，600 kbps
AI：3 FPS，阈值 0.35
缓冲：VI/RGA 8，编码包队列 8
传感器：IMX415 1944x1097 RAW12，驱动有效裁剪 1920x1080，30 FPS
ISP：程序不主动开关 TNR；当前板端 IQ 配置为 TNR ON（2to1）
ALSC：1080p 模式旁路；板载 IQ 文件仅有 3840x2160 镜头阴影标定
```

板载旧版 `librtsp` 在 TCP interleaved、主流 8 Mbps 时会出现 CSeq 和 H.264
损坏。2026-10-21 以子流固定 600 kbps、AI/OSD 开启、RTSP over TCP 同时解码两路：

- 5 Mbps 连续 60 秒：主、子流各 1800 帧，退出码 0，无解码或协议警告。
- 6 Mbps 连续 60 秒：主、子流各 1800 帧，退出码 0，无解码或协议警告。
- 6 Mbps 连续 120 秒：主、子流各 3600 帧，客户端约 30.13 FPS；服务端
  `pts_gap=0`、`queue_high_water=2/8`、`stale_drop=0`。

以上结果均为 TNR 关闭时的历史基线。恢复 TNR 后进行了同板、同模型、AI/OSD
开启的 60 秒板端本机 UDP 对照：

| 主流码率 | TNR | 主流 | 子流 | 服务端 PTS | 结论 |
| --- | --- | --- | --- | --- | --- |
| 5 Mbps | 开 | 1732 帧，29.45 FPS | 1746 帧，29.47 FPS | 约每秒增加 1 个 gap | 未达到 30 FPS |
| 4 Mbps | 开 | 1719 帧，29.53 FPS | 1746 帧，29.46 FPS | 约每秒增加 1 个 gap | 未改善；当次主流另有 RTP 丢包 |
| 5 Mbps | 关 | 服务端约 30 FPS | 服务端约 30 FPS | 稳态 0 | 严格帧率回退 |

4 Mbps 已按当前画质/带宽取舍设为默认值，但它不能解决 4K TNR 带来的帧率缺口。
TNR 位于 VENC 和 RTSP 之前；降低 H.264 目标码率只会减轻编码输出和网络带宽，
不能提高 ISPP/TNR 产帧速度。因此 1.3.0 默认直接选择驱动已有的 1080p@30 模式。
真机在一次主动重启前已连续统计超过 17,000 帧/路，稳态 `pts_gap=0`；
ADB TCP 双流 60 秒各解码 1800 帧、退出码 0。还需完成 30 分钟/24 小时
稳定性测试，才能认定长期结果。

2026-10-22 的 `/proc/rkispp0` 证据：输入、scale0 和 scale1 均为
32–33 ms/帧，delay 约 14–15 ms，TNR 2to1 耗时 2–3 ms，`ErrCnt:0`。
单主流 ADB 包级抓取 60 秒后，稳态 RTP PTS 没有倒退或大于 1.5 帧周期
的间隔；旧 `librtsp` 在会话刚加入时仍可发送 1 个 IDR 前旧包。

## RTP/UDP 传输策略

RTSP 控制连接仍使用 TCP 8554；H.264 媒体默认由客户端在 `SETUP` 中请求
`RTP/AVP/UDP`，以避免 TCP interleaved 的队头阻塞。板载 2020 版静态库已确认包含
UDP RTP/RTCP socket 和 `client_port/server_port` 协商实现，不需要更换服务端程序。

板端本机 FFmpeg 双路 UDP 30 秒测试均正常退出，服务端日志确认主、子流分别建立
UDP RTP/RTCP 端口。无额外接收缓冲时主流曾报告一次丢9个 RTP包；单主流使用
2 MiB socket buffer、512包重排队列和200 ms最大重排等待复测30秒无丢包。
这说明 UDP消除了发送阻塞，但网络拥塞时可能丢包；因此保留 TCP作为回退方式，
并保持主流4 Mbps，不因改用 UDP恢复到已知不稳定的8 Mbps。

VMware NAT 内的 Ubuntu 经 Windows 热点再访问开发板是双 NAT。实测
主、子 UDP 流约 29 秒后同时停止收包，而服务端仍为 30 FPS、
`pts_gap=0`，因此这条路径不可用作 UDP 性能验收。使用 Windows 宿主机直连
开发板热点地址，或将 Ubuntu 改为与开发板同二层的桥接网络。

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

程序不调用 RKAiq TNR 开关 API，由板端 IQ 配置决定 TNR 状态。旧 4K 模式下
`/proc/rkispp0` 显示 `TNR ON (mode: 2to1)`，scale0/scale1 常见约 34–35 ms/帧和
约 160–170 ms 管线 delay，与约 29 FPS 和稳态 PTS 缺口一致。默认 1080p 模式应
显示 1920×1080 输入；必须保存新的处理间隔和 delay 作为验收证据。当前版本没有
`--disable-tnr` 参数。

板载 IQ XML 的 ALSC `resolutionAlll` 只有 `3840x2160`。程序在 1080p 模式下通过
sysctl 同时停用 ALSC 算法与 ISP LSC 模块，而不是错误复用或缩放 4K 标定表；不能
使用此 BSP 中返回成功却不执行设置的 `rk_aiq_user_api_alsc_SetAttrib()`。启动准备
阶段可能出现少量查表提示，但出帧后不应再逐帧出现 `can't find 1920x1080 in
lscResName`。旁路避免错误校正，但可能使镜头边缘亮度/颜色一致性下降，正式量产需
重新标定 IQ。

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
1080p TNR、4 Mbps、AI/OSD 与 Wi-Fi TCP 双流同时运行时，10 秒 `/proc`
采样得到应用占四核总容量 13.57%、整机 busy 21.40%；RSS/HWM 约
120104 kB、33 线程，CPU/NPU 温度约 56.7/55.2°C。这是短时环境值，
不能代替指定环温下的长时稳态测量。

短时空载流 A/B 中，TNR 开启相对关闭使应用占比约增加 2.79 个百分点、整机 busy
约增加 8.03 个百分点，CPU 温度约增加 0.9°C。采样起始温度和 NPU 动态频率并非
完全一致，因此这些数值只用于判断趋势。板端没有可读的电流或功率传感器，不能从
CPU 百分比换算出瓦数；整机电功耗应以 USB 串联功率计在相同场景稳态记录至少 5 分钟。

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

