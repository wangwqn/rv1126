# 运行时分段时延

## 开启和抓取

程序默认不采集时延，避免生产运行时增加无意义的统计开销。调试时添加
`--latency-stats`，每 5 秒会在普通计数器后输出滚动汇总：

```bash
cd /root/rv1126_objects
./rkmedia_object_detection_rtsp \
  --sensor-mode 1080p --rotation 270 --threshold 0.35 --latency-stats
```

输出采用 `P50/P95/P99/max`，单位毫秒；`n` 是当前滚动窗口内样本数，`total`
是启动后累计有效样本数。窗口最多保留最近 512 个样本。硬件图启动后的前三秒
属于预热期，不进入统计，避免 VI/RGA 中构图期间保留的旧帧污染稳态结果。

需要保存每一个样本时使用 `--latency-csv`。它会自动开启 `--latency-stats`：

```bash
./rkmedia_object_detection_rtsp \
  --sensor-mode 1080p --rotation 270 --threshold 0.35 \
  --latency-csv /tmp/latency.csv
```

CSV 格式为：

```text
observed_monotonic_us,frame_pts_us,stage,latency_us,latency_ms
```

- `observed_monotonic_us`：样本完成时的单调时钟，可用于绘制横轴。
- `frame_pts_us`：对应 MEDIA_BUFFER 帧的 PTS，跨阶段关联同一帧。
- `stage`：下表定义的阶段名称。
- `latency_us/latency_ms`：同一数值的整数微秒和便于查看的毫秒表示。

业务线程不会直接写文件，而是把定长记录放进容量 8192 的有界队列，由独立线程
批量写盘并每秒刷新。终端会显示 `csv(written/dropped/queued/high_water/error)`；
有效测试必须满足 `dropped=0` 和 `error=0`。队列满时只丢诊断记录，不阻塞视频。
板端 `/tmp` 是内存文件系统，长测 CSV 可能达到约 50–80 MB/小时，不适合无限采集。

需要把应用日志和 ISPP/TNR 驱动快照一起保存时，在 Ubuntu 仓库根目录执行：

```bash
bash scripts/capture_runtime_latency.sh 60
```

脚本要求当前没有另一个实例占用摄像头，默认把结果写入 Ubuntu 的 `/tmp`，也可以
用第二个参数指定输出目录。它会从开发板拉取逐样本 CSV，生成全量百分位表和 SVG：

```text
application.log          完整程序日志
latency-summary.txt      程序每 5 秒的滚动汇总
latency.csv              每个有效样本
latency-percentiles.csv  根据整个 CSV 重新计算的百分位
latency.svg              14 个阶段的时序小多图
rkispp-before/after.txt  ISPP/TNR 驱动快照
```

也可以手动拉取和绘图；绘图只依赖 Python 3 标准库：

```bash
adb pull /tmp/latency.csv ./latency.csv
python3 scripts/plot_latency_csv.py ./latency.csv ./latency.svg \
  > ./latency-percentiles.csv
```

终端汇总只使用最近 512 个样本，绘图脚本输出的百分位使用 CSV 中的全部样本。
绘图时每个阶段最多显示 1200 个保留局部极值的点，避免大文件生成过大的 SVG；
百分位计算仍使用全部数据。脚本不会把临时日志自动提交到 Git。

## 指标边界

| 指标 | 起点 | 终点 | 实际包含的阶段 |
| --- | --- | --- | --- |
| `ai.rga_age` | AI 帧的 MEDIA_BUFFER PTS | RGA2 BGR 输出被应用取到 | 传感器/ISP/TNR、子 VI、RGA BGR |
| `ai.queue` | AI 帧放入最新帧队列 | AI 线程取出 | 线程唤醒和队列等待 |
| `ai.infer` | 调用检测器前 | RKNN、解码和 NMS 返回后 | 预处理、NPU 推理、后处理 |
| `ai.total` | AI 帧 PTS | 检测结果完成 | `rga_age + queue + infer` 等完整 AI 路径 |
| `osd.wait` | 检测完成 | OSD 更新开始 | 结果发布、条件变量唤醒和线程调度 |
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

## 2026-10-23 逐样本 CSV 真机验证

在相同的 IMX415 1080p RAW12、TNR 2to1、主流 4 Mbps、子流 600 kbps、
30 FPS、AI 3 FPS 和 OSD 开启条件下，执行 60 秒一键抓取。程序启动、模型加载和
3 秒预热不计入有效样本；CSV 最终包含 13,600 条有效记录，14 个阶段全部存在。
异步写线程报告 `written=13600`、`dropped=0`、`error=0`，队列最高水位仅
`8/8192`。主、子流本轮分别输出 1521/1522 帧，稳态均为 `pts_gap=0`。

下表由 `plot_latency_csv.py` 对 CSV 全部样本重新计算，不是 512 点滚动窗口：

| 阶段 | 样本数 | P50 | P95 | P99 | 最大值 |
| --- | ---: | ---: | ---: | ---: | ---: |
| `ai.rga_age` | 1432 | 22.484 ms | 23.714 ms | 24.358 ms | 26.519 ms |
| `ai.queue` | 144 | 0.077 ms | 0.479 ms | 0.664 ms | 0.686 ms |
| `ai.infer` | 144 | 15.739 ms | 17.514 ms | 18.138 ms | 18.231 ms |
| `ai.total` | 144 | 38.344 ms | 40.858 ms | 42.131 ms | 42.372 ms |
| `osd.wait` | 143 | 10.752 ms | 19.245 ms | 20.162 ms | 20.181 ms |
| `osd.update` | 143 | 0.060 ms | 0.103 ms | 0.144 ms | 0.161 ms |
| `main.venc_age` | 1432 | 32.551 ms | 34.586 ms | 37.217 ms | 44.792 ms |
| `main.rtsp_wait` | 1431 | 0.118 ms | 0.849 ms | 1.143 ms | 1.481 ms |
| `main.send` | 1431 | 0.650 ms | 1.381 ms | 6.311 ms | 7.933 ms |
| `main.server_total` | 1431 | 33.432 ms | 35.651 ms | 43.484 ms | 49.231 ms |
| `sub.venc_age` | 1432 | 22.568 ms | 33.947 ms | 34.913 ms | 42.123 ms |
| `sub.rtsp_wait` | 1431 | 0.126 ms | 0.852 ms | 1.227 ms | 2.197 ms |
| `sub.send` | 1431 | 0.204 ms | 0.531 ms | 1.619 ms | 1.998 ms |
| `sub.server_total` | 1431 | 23.143 ms | 34.759 ms | 35.938 ms | 43.716 ms |

这次验证证明逐样本记录和绘图链路可用，且测试期间没有对媒体队列造成可见背压；
它仍然只覆盖板端服务路径，不能据此推导 Wi-Fi、客户端解码和显示后的端到端时延。

## 2026-10-23 事件驱动 OSD 验证

1.5.1 将固定 20 ms 轮询替换为检测结果发布时的条件变量通知。相同真机配置和
60 秒抓取下，CSV 共 13,656 条有效记录，`dropped=0`、`error=0`，主、子流
各 1528 帧且 `pts_gap=0`。OSD 路径的全量样本对比如下：

| 指标 | 1.5.0 P50/P95/P99/最大值 | 1.5.1 P50/P95/P99/最大值 |
| --- | --- | --- |
| `osd.wait` | 9.178/18.741/20.042/20.191 ms | 0.115/0.240/0.637/0.889 ms |
| `osd.update` | 0.060/0.151/0.212/0.214 ms | 0.025/0.046/0.055/0.063 ms |

`osd.wait` P99 降低约 96.8%，同时避免 AI 只有 3 FPS 时 OSD 线程每秒 50 次轮询。
短时 CPU A/B 存在系统噪声，尚不能把理论唤醒次数下降表述为可测的整机 CPU 降幅。

## 2026-10-23 AI 工作区复用 A/B

1.5.2 将 `320×320×3` letterbox 图像和解码候选框作为 `Model` 内部工作区
复用。输入宽高不变时，只改写 resize ROI；宽高或宽高比变化时会先将整张
图恢复为 114，不会暴露上一种 ROI 的旧像素。最终检测框仍由返回的
`Detection` 独立持有，可以安全跨线程发布。

两个二进制在同一开发板、同一配置下先后各采集 60 秒，每轮都获得
144 个 `ai.infer` 样本和 13,656 条总 CSV 记录：

| 版本 | P50 | P95 | P99 | 最大值 |
| --- | ---: | ---: | ---: | ---: |
| 1.5.1 基线 | 15.655 ms | 16.963 ms | 17.827 ms | 17.873 ms |
| 1.5.2 工作区复用 | 15.529 ms | 16.786 ms | 17.567 ms | 17.574 ms |

候选版主、子流各 1528 帧，`pts_gap=0`，CSV `dropped=0`、`error=0`。
改善幅度约 0.8%–1.7%，主要是减少 CPU 预/后处理的分配和填充，不应解读为
NPU 运算本身加速。

## 不能由板端统计代替的时延

`server_total` 的终点只是发送 API 返回，不包含 Wi-Fi 排队、RTP 重传或丢包、
客户端 socket、H.264 解码、VLC/FFplay 缓存和显示器刷新。因此它不是端到端时延。
完整端到端仍需让摄像头拍摄毫秒计时器或 LED，同时拍摄真实场景与播放窗口，记录
至少 100 个样本后计算 P50/P95/P99。客户端低缓存参数只能减少缓冲，不能证明
“端到端低于 200 ms”。
