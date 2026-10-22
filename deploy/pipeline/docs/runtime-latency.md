# 运行时分段时延

## 开启和抓取

程序默认不采集百分位，避免生产运行时增加无意义的统计开销。调试时添加
`--latency-stats`，每 5 秒会在普通计数器后输出一行：

```bash
cd /root/rv1126_objects
./rkmedia_object_detection_rtsp \
  --sensor-mode 1080p --rotation 270 --threshold 0.35 --latency-stats
```

输出采用 `P50/P95/P99/max`，单位毫秒；`n` 是当前滚动窗口内样本数，`total`
是启动后累计有效样本数。窗口最多保留最近 512 个样本。硬件图启动后的前三秒
属于预热期，不进入统计，避免 VI/RGA 中构图期间保留的旧帧污染稳态结果。

需要把应用日志和 ISPP/TNR 驱动快照一起保存时，在 Ubuntu 仓库根目录执行：

```bash
bash scripts/capture_runtime_latency.sh 60
```

脚本要求当前没有另一个实例占用摄像头，默认把结果写入 `/tmp`。也可以用第二个参数
指定输出目录。脚本不会把临时日志提交到 Git。

## 指标边界

| 指标 | 起点 | 终点 | 实际包含的阶段 |
| --- | --- | --- | --- |
| `ai.rga_age` | AI 帧的 MEDIA_BUFFER PTS | RGA2 BGR 输出被应用取到 | 传感器/ISP/TNR、子 VI、RGA BGR |
| `ai.queue` | AI 帧放入最新帧队列 | AI 线程取出 | 线程唤醒和队列等待 |
| `ai.infer` | 调用检测器前 | RKNN、解码和 NMS 返回后 | 预处理、NPU 推理、后处理 |
| `ai.total` | AI 帧 PTS | 检测结果完成 | `rga_age + queue + infer` 等完整 AI 路径 |
| `osd.wait` | 检测完成 | OSD 更新开始 | 结果缓存和 20 ms OSD 轮询等待 |
| `osd.update` | 调用 RGA region 更新前 | 更新返回 | 坐标缩放后的 region/标签配置调用 |
| `main/sub.venc_age` | 视频帧 PTS | H.264 MEDIA_BUFFER 被取到 | 传感器/ISP/TNR、VI、RGA、VENC |
| `main/sub.rtsp_wait` | 编码包取到 | RTSP 线程开始发送 | 无损队列等待和发送线程调度 |
| `main/sub.send` | 调用 `rtsp_tx_video` 前 | 调用返回 | 旧 `librtsp` 的服务端发送调用 |
| `main/sub.server_total` | 视频帧 PTS | `rtsp_tx_video` 返回 | 板端到网络发送 API 返回的完整路径 |

这些名称刻意区分“可观察边界”和“单个硬件模块耗时”。当前 RKMedia 使用硬件
`Bind`，应用没有机会在 VI→RGA 和 RGA→VENC 的每个入口插入软件时间戳；因此
`venc_age` 不能解释成 VENC 单模块耗时，`rga_age` 也不能解释成 RGA 单模块耗时。
为了得到硬件内部证据，需要同时读取：

```bash
adb shell 'cat /proc/rkispp0'
```

其中 `Input/Output rate` 是帧周期，`delay` 是驱动报告的管线延迟，`TNR time`、
`NR time` 是对应硬件块的处理时间。应用统计与驱动统计口径不同，不能直接逐项相加。

## 2026-10-22 真机样本

条件：IMX415 1080p RAW12、TNR 2to1、主流 4 Mbps、子流 600 kbps、30 FPS、
AI 3 FPS、OSD 开启。512 样本滚动窗口的一轮稳态结果如下：

| 路径 | P50 | P95 | P99 | 最大值 |
| --- | ---: | ---: | ---: | ---: |
| AI RGA 输出帧年龄 | 22.78 ms | 24.67 ms | 25.30 ms | 25.83 ms |
| AI 队列等待 | 0.07 ms | 0.52 ms | 0.74 ms | 1.40 ms |
| AI 推理与后处理 | 16.04 ms | 17.44 ms | 18.25 ms | 19.32 ms |
| AI 总路径 | 38.59 ms | 40.99 ms | 42.28 ms | 45.20 ms |
| OSD 等待 | 10.82 ms | 19.27 ms | 19.93 ms | 20.15 ms |
| OSD 更新调用 | 0.58 ms | 1.36 ms | 1.75 ms | 2.33 ms |
| 主流编码输出帧年龄 | 35.12 ms | 38.48 ms | 40.70 ms | 41.59 ms |
| 主流 RTSP 队列等待 | 0.07 ms | 0.74 ms | 1.05 ms | 1.17 ms |
| 主流 RTSP 发送调用 | 0.64 ms | 0.89 ms | 6.69 ms | 9.67 ms |
| 主流服务端总路径 | 35.91 ms | 40.46 ms | 45.75 ms | 48.40 ms |
| 子流编码输出帧年龄 | 22.14 ms | 27.63 ms | 33.67 ms | 35.19 ms |
| 子流 RTSP 队列等待 | 0.07 ms | 0.59 ms | 0.85 ms | 1.17 ms |
| 子流 RTSP 发送调用 | 0.17 ms | 0.30 ms | 1.51 ms | 2.25 ms |
| 子流服务端总路径 | 22.76 ms | 28.22 ms | 34.29 ms | 36.30 ms |

本轮 `invalid_pts=0`，说明 MEDIA_BUFFER PTS 与 `steady_clock` 的单调时钟基准可直接
比较；主、子流累计超过 75,000 帧时稳态 `pts_gap=0`，RTSP 队列高水位为 2/8。
驱动快照显示 ISPP 输入/输出周期约 32 ms、输出 delay 约 15 ms、TNR 约 3 ms。

## 不能由板端统计代替的时延

`server_total` 的终点只是发送 API 返回，不包含 Wi-Fi 排队、RTP 重传或丢包、
客户端 socket、H.264 解码、VLC/FFplay 缓存和显示器刷新。因此它不是端到端时延。
完整端到端仍需让摄像头拍摄毫秒计时器或 LED，同时拍摄真实场景与播放窗口，记录
至少 100 个样本后计算 P50/P95/P99。客户端低缓存参数只能减少缓冲，不能证明
“端到端低于 200 ms”。
