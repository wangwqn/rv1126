# RV1126 + IMX415 性能验收

本清单用于验证三个目标：主流 `1920x1080@30FPS` 无应用可见丢帧、进程 CPU
低于 30%、端到端延迟低于 200 ms。三项都必须在真实开发板、真实网络和指定
客户端上测量；交叉编译通过不等于性能达标。

## 1. 部署前记录

在开发板上记录：

```sh
uname -a
cat /proc/device-tree/model
media-ctl -p
v4l2-ctl --list-devices
```

确认 IMX415 连接的 ISPP 节点是 `rkispp_scale0` 和 `rkispp_scale1`，并确认二者
来自同一传感器和相同视场。检查没有启用 `/tmp/.rkmedia_vi_debug` 一类会让 SDK
强制使用 MMAP 的调试开关。

## 2. 启动

```sh
adb push build-rv1126-sdk/rkmedia_rockx_face_detection_rtsp /root/
adb shell chmod +x /root/rkmedia_rockx_face_detection_rtsp
adb shell 'cd /root && ./rkmedia_rockx_face_detection_rtsp \
  --width 1920 --height 1080 --sub-width 640 --sub-height 360 \
  --fps 30 --ai-fps 5 --bitrate 8000000 --sub-bitrate 800000 \
  --pool-buffers 8 --packet-queue 8'
```

启动日志必须显示 VI、RGA、VENC 两路均创建成功。`V4L2_MEMORY_DMABUF` 是否真正
生效可用 RKMedia 调试日志或驱动 trace 验证；不能只依据代码配置作结论。

程序每 5 秒输出统计。稳定网络下要求：

- `main.pts_gap=0`；
- `sub.pts_gap=0`；
- `ai.source` 接近视频帧数、`ai.sampled` 接近视频帧数的六分之一；
- `rtsp.queue_high_water` 明显低于 `8`，不能长期顶满；
- AI 慢时允许 `ai.stale_drop` 增长，因为只淘汰 AI 帧，不是视频帧；
- `shutdown_abandoned` 只表示停止瞬间主动清理的包，不计入运行期丢帧。

## 3. 输出帧率与连续性

在同一局域网 Linux/Ubuntu 客户端执行：

```sh
ffprobe -v error -rtsp_transport tcp -select_streams v:0 \
  -show_entries stream=codec_name,profile,width,height,r_frame_rate,avg_frame_rate \
  -of default=noprint_wrappers=1 \
  rtsp://<开发板IP>:8554/live/main_stream

ffmpeg -hide_banner -loglevel warning -rtsp_transport tcp \
  -i rtsp://<开发板IP>:8554/live/main_stream \
  -t 1800 -an -f null -
```

至少持续 30 分钟，同时播放子流。通过条件是主流为 1920x1080 H.264、约 30 FPS，
FFmpeg 无解码错误，程序 `main.pts_gap=0`。再做 24 小时测试验证温度、降频、网络
重连和 DMA-BUF 生命周期。绝对“永不丢帧”无法用有限测试证明，报告应写明测试
时长、网络条件和观测范围。

## 4. CPU 占用

取得进程 PID 后，先预热 60 秒，再连续采样 10 分钟：

```sh
pidof rkmedia_rockx_face_detection_rtsp
top -b -d 1 -n 600 -p <PID>
```

若 BusyBox `top` 不支持这些参数，可每秒读取 `/proc/<PID>/stat` 与 `/proc/stat`，
按时钟 tick 差值计算。报告必须明确口径：Linux `top` 的 100% 通常表示占满一个
CPU 核。本项目目标“低于 30%”按单进程 `top %CPU` 的 10 分钟平均值验收，同时
记录峰值、SoC 温度和频率。

分别测四组以定位开销：

```text
默认（AI+OSD）
--no-osd
--no-ai --no-osd
只播放主流客户端
```

如果 `--no-ai --no-osd` 仍高于 30%，重点检查 VI 是否回退 MMAP、RGA/VENC 是否
实际硬件运行、RTSP TCP 发送开销和调试日志。若只有默认模式超标，则重点检查
RockX CPU 映射、模型帧率及子流 BGR->NV12 转换。

## 5. 端到端延迟

播放器命令：

```sh
ffplay -rtsp_transport tcp -fflags nobuffer -flags low_delay \
  -framedrop -probesize 32 -analyzeduration 0 -sync ext \
  rtsp://<开发板IP>:8554/live/main_stream
```

端到端必须包含传感器曝光、ISP、RGA、VENC、RTSP、网络、解码和显示。建议让
IMX415 拍摄毫秒计时器或周期闪烁 LED，用另一台高帧率相机同时拍摄真实目标与
客户端屏幕，逐帧求差。至少统计 1000 个样本并报告 P50/P95/P99；通过条件建议为
P95 < 200 ms，且 P99 无持续尖峰。用手挥动或主观观察不能作为验收数据。

若超标，依次检查：曝光时间、客户端缓存、`queue_high_water`、Wi-Fi 抖动、解码器
缓冲和显示垂直同步。不要通过无界缓存换取零丢帧；正常运行时包队列高水位应低，
一旦接近 8，延迟目标已经有风险。

## 6. 资源与故障

长测期间记录：

```sh
grep -E 'VmRSS|Threads' /proc/<PID>/status
ls /proc/<PID>/fd | wc -l
cat /sys/kernel/debug/dma_buf/bufinfo
dmesg | tail -100
```

DMA-BUF 总数和 RSS 在预热后应保持有界。反复执行 100 次启动、播放、SIGINT、
重新启动，确认 RTSP 端口、VI/RGA/VENC 通道和 DMA 缓冲均能释放。网络断开或慢
客户端可能触发背压和最终停滞错误，这是有界低延迟系统的保护行为；记录恢复
策略，不应将其误报为“零丢帧仍正常运行”。
