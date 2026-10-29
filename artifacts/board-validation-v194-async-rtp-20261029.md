# v1.9.4 异步 RTSP/RTP 与 Wi-Fi 验收记录

日期：2026-10-29（Asia/Shanghai）
开发板：RV1126 + IMX415
板端地址：`192.168.137.176`
媒体路径：宿主机 `wang` 热点 -> VMware 桥接 -> Ubuntu -> Wi-Fi RTSP/TCP
测试中未使用 ADB 转发；ADB 只用于部署和板端遥测。

## 1. 应用层 RTSP 替换

历史的 2020 年 `librtsp` 在 TCP interleaved 长时运行中会损坏 H.264
和 CSeq。尝试切换到 SDK 内置 Rockchip 2019 live555 后，5分钟双流
降到约 28.11 FPS，并出现 `CSeq 62 expected, 0 received`。

v1.9.4 候选因此改为项目自有的非阻塞 RTSP/RTP-over-TCP 服务端：

- 每客户端独立 CSeq、Session、RTP sequence、SSRC 和输出队列；
- 支持 `OPTIONS/DESCRIBE/SETUP/PLAY/PAUSE/TEARDOWN/GET_PARAMETER`；
- H.264 Annex-B 解析与 RFC 6184 单 NAL/FU-A 分包；
- PCMA/8000/1 RTP track；
- `PLAY` 主动请求 VENC IDR，且起播前丢弃 P 帧；
- 一帧的所有 RTP interleaved 记录合并为一个连续缓冲；
- 慢客户端队列超限后不断开 Session，而是清除过期 RTP，等待
  socket 恢复可写后请求一次 IDR。

## 2. 已通过的功能与短时测试

### 2.1 基本码流

- `/live/main_stream`：H.264，1080x1920（旋转后），30/1 FPS + PCMA 8 kHz。
- `/live/sub_stream`：H.264，360x640（旋转后），30/1 FPS + PCMA 8 kHz。
- AI、多目标 OSD、音频、TNR、主子流与看门狗同时开启。

### 2.2 稳态帧率

- 60秒：主流1800帧/30.00 FPS，子流1801帧/30.02 FPS，通过。
- 5分钟：主流9001帧/30.00 FPS，子流9000帧/30.00 FPS，通过。
- 10分钟（3.5 Mbps 对照）：主流18001帧，子流18000帧，通过。
- 10分钟（3 Mbps + AF41/WMM + 2 MiB 候选）：主流18001帧，
  子流18000帧，通过。
- 上述通过轮次未出现 CSeq、Invalid NAL、RTP missed 或解码错误。

### 2.3 可控拥塞恢复

宿主机暂停主流 `ffprobe` 读取20秒。修正后板端只记录：

```text
rtsp: stream=0 backpressure recovery=1 retained=0 bytes
rtsp: stream=0 transport writable; requesting IDR
```

同一客户端 PID 恢复后又收到1099帧；客户端错误日志为空，PTS保留
13.4秒的真实网络缺口，没有伪造连续时间戳。

## 3. 性能与板端健康

多次正式验收采样中：

- 应用 CPU（四核总容量）：约13.66%--19.21%；
- 线程：35；
- FD：159--162；
- RSS：约175--238 MiB，未见持续单调增长；
- CPU 温度：约62--66摄氏度；
- 看门狗重启：0；
- RKISPP `ErrCnt=0`；
- VENC `pts_gap=0`；
- AI completed、OSD boxes、audio captured/sent 计数持续增长。

## 4. WLAN 与 TCP 优化

开发板与 `wang` 热点信号约-47~-50 dBm，tx bitrate 72 Mbps。实测
cfg80211 `Power save` 会约每40--80秒短暂重新开启。已增加：

- 管理脚本每秒重申 `iwconfig wlan0 power off`；
- `iw dev wlan0 set power_save off`；
- Broadcom `wl -i wlan0 PM 0`；
- RTSP socket `TCP_NODELAY`；
- 64 KiB `SO_SNDBUF`；
- AF41 `IP_TOS` 和 `SO_PRIORITY=5`，映射到 Wi-Fi WMM video AC；
- 有界慢客户端恢复。

## 5. 正式30分钟结论

正式结论：**不通过**。

已保留的主要轮次：

- `/tmp/rv1126-acceptance-30m-async-rtp-20261029-015909`：宿主网络不可达，作废。
- `/tmp/rv1126-acceptance-30m-backpressure-20261029-021311`：10分钟主流
  17579帧/29.30 FPS，子流18000帧/30.00 FPS；网络不可达采样1次。
- `/tmp/rv1126-acceptance-30m-v194-3500k-20261029-075209`：第3分钟
  主流背压恢复，终止。
- `/tmp/rv1126-acceptance-30m-v194-final-20261029-084420`：第2分钟
  主流队列上升到约1.95 MiB并触发恢复，终止。

最终发布配置（4 Mbps、512 KiB客户端队列）重新部署后，板端健康检查为
主/子流3秒各增加93帧、AI/OSD/音频/TNR全部开启、`ErrCnt=0`、
`WLAN_PS=off`。随后进行60秒Wi-Fi双流复测：主流1721帧/28.68 FPS，
子流1800帧/30.00 FPS。主流客户端无H.264/CSeq解码错误，板端VENC仍为
`pts_gap=0`，但RTSP日志记录一次275274字节队列高水位和一次背压恢复。
这次复测未通过，并再次证明丢帧发生在主流TCP传输路径，而非采集或编码端。

降低主流码率、改发送缓冲、关闭省电、WMM优先级和扩大队列都
只能减少或吸收短暂拖延，不能消除 Windows 热点经 VMware 桥接到
Ubuntu 时的随机数秒级主流 TCP 停顿。继续扩大队列会与200 ms低时延
目标相冲，因此已停止。

## 6. 12小时验收状态

**未启动**。启动12小时测试的前置条件是30分钟正式验收通过。
建议先使用以下任一网络路径重测：

1. Windows宿主机直接运行 FFmpeg/VLC，绕过 VMware 桥接；
2. 开发板与 Ubuntu 连接同一个稳定独立 AP；
3. 有线以太网；
4. 若业务允许丢包换取无 TCP 队头阻塞，另行验证 RTP/UDP。

Windows宿主机正式30分钟命令：

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\test_rtsp_tcp_windows.ps1 `
  -BoardIp 192.168.137.176 -DurationSeconds 1800 -ExpectedFps 30
```

上述网络中30分钟通过后，再运行：

```bash
bash scripts/test_8h_acceptance.sh 192.168.137.176 43200 600 \
  /tmp/rv1126-acceptance-12h-$(date +%Y%m%d-%H%M%S)
```
