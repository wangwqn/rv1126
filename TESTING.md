# 物品检测测试流程

若程序已经运行，先测试播放，不要重复启动另一个摄像头程序。

## 1. Ubuntu 检查开发板和网络

```bash
adb devices
adb shell 'connmanctl services | head -2; ip -4 addr show wlan0'
adb shell 'pidof rkmedia_object_detection_rtsp; netstat -lnt 2>/dev/null | grep :8554'
```

设备应显示为 `device`；Wi-Fi 地址必须每次从开发板重新查询，不沿用旧 DHCP 地址。
需要检查 AI 计数时以前台方式运行；每 5 秒日志中的 `ai(sampled/completed)` 应增长。
检测数为 0 只表示当前帧没有目标通过阈值，不代表模型没有运行。

## 2. Windows 宿主机播放主流

PowerShell：

```powershell
Test-NetConnection <开发板当前IP> -Port 8554
```

预期 TcpTestSucceeded=True。VLC 按 Ctrl+N，输入：

```text
rtsp://<开发板当前IP>:8554/live/main_stream
```

主流显示检测框和中文名称；子流 `/live/sub_stream` 当前是无标注预览。
以上地址依赖当次 DHCP 结果，重启后请先查地址。

VLC 默认应使用 UDP 媒体通道：工具 → 偏好设置 → 输入/编解码器，取消勾选
“RTP over RTSP (TCP)”并重启播放。Windows 防火墙应允许 VLC 接收专用网络 UDP；
RTSP 控制连接仍通过 TCP 8554，所以 `Test-NetConnection` 只验证控制端口。

## 3. UDP 双流验收

Ubuntu 必须能经 Wi-Fi/有线网络直接访问开发板 IP，不能使用 ADB 端口转发：

```bash
bash scripts/test_rtsp_udp.sh <开发板当前IP> 60
```

脚本同时解码主流和子流，并检查 `RTP: missed`、H.264 损坏、超时和连接错误。
脚本的墙钟上限为“指定时长 + 20 秒”，媒体包中断时会失败退出，不会永久等待。
不要从 VMware NAT 内的 Ubuntu 经 Windows 热点 NAT 验收 UDP；该双 NAT
链路实测约 29 秒后丢失 RTP 映射。使用 Windows 宿主机直连或桥接网络。

## 4. USB ADB 的 TCP 回退诊断

局域网 TCP 双流帧率验收：

```bash
bash scripts/test_rtsp_tcp.sh <开发板当前IP> 60
```

脚本会先等待主流收到视频帧，再建立子流会话，避免旧版 `librtsp`
同时处理两个 RTSP SETUP 握手时偶发卡住。日志保存在 `/tmp/rv1126_tcp_test/`。
为避免启动 PTS 跳变干扰，脚本按真实墙钟区间统计解码帧。结束时的
`exit_code=143` 是脚本到达指定时间后主动停止 ffprobe，不是异常；以最终
`PASS/FAIL`、帧数和错误扫描为准。
当前正式配置为 2×2 binning 1080p@30，验收时使用默认的30 FPS：

```bash
bash scripts/test_rtsp_tcp.sh <开发板当前IP> 60 30
```

```bash
adb forward tcp:8554 tcp:8554
ffplay -rtsp_transport tcp rtsp://127.0.0.1:8554/live/main_stream
```

ADB forward 只转发 TCP，不能承载协商出的 RTP/UDP 端口。该命令只用于 UDP 不通时
隔离诊断；如果它正常而 Windows UDP 失败，优先排查热点、路由和 Windows 防火墙。
Windows 上不能直接使用 Ubuntu 的 127.0.0.1 转发地址。

## 5. 音视频轨道验收

主、子流都应包含 H.264 视频和 8 kHz 单声道 G.711A/PCMA 音频：

```bash
bash scripts/test_rtsp_av.sh <开发板当前IP> 10
```

脚本会检查两条 Session 的 SDP，实际解码主流视频和音频，并确认音频样本不是静音。
运行日志中的 `audio(captured/sent)` 应以约 50 包/秒增长，正常情况下
`queue_drop=0`、`recoveries=0`。只测试视频时可添加 `--no-audio`。

## 6. 检测功能

依次放入手机、杯子、瓶子、书籍、鼠标、键盘、笔记本电脑、显示器、香蕉、苹果、橙子。
每次先只放一个，靠近镜头且完整可见，光照充足。然后测试多目标、遮挡、远近和背景变化。

记录：是否检测到、类别是否正确、框是否贴合、名称是否可读、移走物品后框是否消失。
正式启动脚本的置信度阈值为0.25（程序内建值为0.35）；不要把降低
阈值后出现更多框当成精度提高。
当前已观察到杯子/瓶子混淆，书籍和部分水果较弱，需继续训练改进。

23类正式模型已完成板端公开图片和短时实时流验证，但还需用实物逐类测试：人、背包、
手提包、碗、三明治、椅子、沙发、盆栽、餐桌、遥控器、时钟和剪刀。
执行 `bash scripts/push_objects23_candidate.sh` 只会上传到
`/root/rv1126_objects23_candidate`，不会覆盖已验证的默认服务。

## 7. 单张图片隔离测试

最好先停止视频后测延时，避免并发竞争影响结果。
开发板已存放测试图，执行：

```bash
adb shell 'cd /root/rv1126_objects && ./object_image objects.rknn objects.json labels.png 000000046031.jpg test-result.jpg 0.35'
adb pull /root/rv1126_objects/test-result.jpg ./test-result.jpg
```

预期能看到笔记本电脑、鼠标、键盘的中文框图。本次已有结果在 `artifacts/device-initial-result.jpg`。

23类候选的已验证命令和结果：

```bash
adb shell 'cd /root/rv1126_objects23_candidate && ./object_image objects.rknn objects.json labels.png test.jpg result.jpg 0.25'
adb pull /root/rv1126_objects23_candidate/result.jpg artifacts/device-objects23-result.jpg
```

本次板端检出人、椅子、笔记本电脑、显示器/电视和杯子；连续三次总耗时
22.037、21.772、21.870 ms，平均 21.893 ms。完整板载结果见
`artifacts/board-validation-objects23-20261024.md`。

候选全视场配置使用：

```bash
adb shell '/root/rv1126_objects23_candidate/rv1126-object23 start'
adb shell '/root/rv1126_objects23_candidate/rv1126-object23 health'
adb shell '/root/rv1126_objects23_candidate/rv1126-object23 stop'
```

它固定为4K传感器采集、1080p/360p输出，实测约29.8 FPS，不应作为严格30 FPS
结果。与1080p传感器模式的性能对比见 `artifacts/full-fov-validation-20261024.md`。
原始3840×2160主码流测试未通过，不应作为默认启动参数；失败详情见
`artifacts/native-4k-output-validation-20261024.md`。

## 8. 需要手动重启时

发布包已包含板端管理脚本，它会同时校验 PID 文件和 `/proc/<pid>/cmdline`，
避免 PID 复用时误停其他进程：

```bash
adb shell '/root/rv1126_objects/rv1126-object status'
adb shell '/root/rv1126_objects/rv1126-object health'
adb shell '/root/rv1126_objects/rv1126-object restart'
adb shell 'tail -100 /tmp/rv1126-object.log'
```

正式管理脚本默认是 2×2 binning 1080p、30 FPS、旋转270度、阈值0.25、
最多8个OSD框，AI、OSD和音频全部开启。如要安装为Buildroot
开机服务（可选，部署脚本不会自动开启）：

```bash
adb shell '/root/rv1126_objects/rv1126-object install-autostart'
```

需要实时查看前台日志时，先 `stop`，再执行：

```bash
adb shell
cd /root/rv1126_objects
./rkmedia_object_detection_rtsp \
  --sensor-mode 1080p \
  --width 1920 --height 1080 --sub-width 640 --sub-height 360 \
  --fps 30 --ai-fps 3 --max-osd-boxes 8 \
  --bitrate 4000000 --sub-bitrate 600000 \
  --pool-buffers 8 --packet-queue 8 --rotation 270 --threshold 0.35 \
  --audio-device plughw:0,0 --audio-source main --audio-period-ms 20
```

保持这个终端运行；退出按 Ctrl+C。不要使用 `--no-ai`、`--no-osd`
或 `--no-audio`。多目标验收时同时放入至少3件已知物品，日志中
`osd(boxes=N, truncated=M)` 的 `N` 应大于1且不大于8。
前台启动后日志显示在当前终端，不再写入先前的后台日志文件。

程序不主动开关 TNR，由板端 IQ 配置决定。默认 `--sensor-mode 1080p` 在 RKAiq
启动前选择 IMX415 的 1944×1097 RAW12 2×2 binning 模式，并将有效记录区域
对齐为 1920×1080；这不是 4K 中心裁剪。启动日志
应只出现一次 `camera: ALSC bypassed`；这是因为板载 IQ 文件没有 1080p 镜头阴影
标定。可用下面命令确认 TNR 输入、状态和处理间隔：

```bash
adb shell 'cat /proc/rkispp0'
```

应看到 1920×1080 输入和 `TNR ON (mode: 2to1)`。需要与旧模式对照时添加
`--sensor-mode 4k`；当前版本没有 `--disable-tnr` 参数。

v1.8.7开始，AI专用RGA2在输入端直接由视频帧率限制到 `--ai-fps`。日志中
`ai(source=..., sampled=...)` 两项应基本相等，并按默认约3次/秒增长；如果仍按
29/30次/秒增长，说明运行的不是当前二进制。输入限速历史对照见
`artifacts/ai-rga-rate-limit-validation-20261025.md`。

部署新版本并等待摄像头管线通过健康检查：

```bash
bash scripts/deploy_release.sh --restart
```

如新版本异常，回退到上一次成功部署：

```bash
bash scripts/rollback_device.sh
```

回滚在停止视频前先用 `DEPLOYED_SHA256SUMS.previous` 校验全部备份；
任何备份缺失或校验失败都会拒绝停止当前服务。文件交换后还会重启服务并
重试健康检查，确认视频管线恢复。

## 9. 故障定位

| 现象 | 首先检查 |
| --- | --- |
| 8554 不通 | 当前 IP、热点、程序是否运行及监听 |
| TCP 可播、UDP 不通 | 客户端是否直连板端 IP、Windows 防火墙、热点是否过滤 UDP |
| UDP 偶发花屏 | 检查 `RTP: missed`；增大接收缓冲或降到 5/4 Mbps，必要时回退 TCP |
| 有视频但没标注 | 是否播放 main_stream，是否禁用 AI/OSD，物体是否属于 11 类 |
| 有视频但没声音 | `amixer -c 0` 的 Capture MIC Path、`/dev/snd/pcmC0D0c`、是否添加 `--no-audio` |
| 单图识别正常，实时没框 | AI 日志、目标大小、旋转、摄像头画面与训练数据的差异 |
| 名称错或漏检 | 模型精度、光照、目标尺度；本模型不是成熟成品 |
| 端口占用/摄像头打开失败 | 是否同时启动了旧人脸程序或第二个新程序 |

模型和数据来源、训练命令见 README.md，实际验证版本见 STATUS.md。

## 10. 分段时延抓取

先停止正在运行的实例，再从 Ubuntu 仓库根目录执行：

```bash
bash scripts/capture_runtime_latency.sh 60
```

脚本使用 `--latency-csv` 运行程序，保存逐样本 CSV、全量 P50/P95/P99/max、SVG
时序图和测试前后的 `/proc/rkispp0` 快照。默认结果目录在 Ubuntu 的 `/tmp`。
各指标的准确起止边界和当前真机基线见
`deploy/pipeline/docs/runtime-latency.md`；其中 `server_total` 只到板端 RTSP 发送
API 返回，不是 VLC 显示端到端时延。
