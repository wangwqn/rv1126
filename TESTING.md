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

## 3. Ubuntu 经 USB ADB 交叉验证

```bash
adb forward tcp:8554 tcp:8554
ffplay -rtsp_transport tcp rtsp://127.0.0.1:8554/live/main_stream
```

如果 ADB 转发播放正常、Windows 失败，优先排查热点/主机到开发板的网络。
Windows 上不能直接使用 Ubuntu 的 127.0.0.1 转发地址。

## 4. 检测功能

依次放入手机、杯子、瓶子、书籍、鼠标、键盘、笔记本电脑、显示器、香蕉、苹果、橙子。
每次先只放一个，靠近镜头且完整可见，光照充足。然后测试多目标、遮挡、远近和背景变化。

记录：是否检测到、类别是否正确、框是否贴合、名称是否可读、移走物品后框是否消失。
现版本置信度阈值 0.35；不要把降低阈值后出现更多框当成精度提高。
当前已观察到杯子/瓶子混淆，书籍和部分水果较弱，需继续训练改进。

## 5. 单张图片隔离测试

最好先停止视频后测延时，避免并发竞争影响结果。
开发板已存放测试图，执行：

```bash
adb shell 'cd /root/rv1126_objects && ./object_image objects.rknn objects.json labels.png 000000046031.jpg test-result.jpg 0.35'
adb pull /root/rv1126_objects/test-result.jpg ./test-result.jpg
```

预期能看到笔记本电脑、鼠标、键盘的中文框图。本次已有结果在 `artifacts/device-initial-result.jpg`。

## 6. 需要手动重启时

先停止本次后台服务（同时核对 PID 文件和可执行文件，避免误停其他程序）：

```bash
adb shell 'start-stop-daemon -K -p /tmp/rkvision-objects.pid -x /root/rv1126_objects/rkmedia_object_detection_rtsp -s INT'
adb shell 'netstat -lnt 2>/dev/null | grep :8554'
```

确认 8554 不再监听后，前台运行：

```bash
adb shell
cd /root/rv1126_objects
./rkmedia_object_detection_rtsp \
  --width 1920 --height 1080 --sub-width 640 --sub-height 360 \
  --fps 30 --ai-fps 3 --bitrate 4000000 --sub-bitrate 600000 \
  --pool-buffers 8 --packet-queue 8 --rotation 270 --threshold 0.35
```

保持这个终端运行；退出按 Ctrl+C。不要使用 --no-ai 或 --no-osd。
前台启动后日志显示在当前终端，不再写入先前的后台日志文件。

## 7. 故障定位

| 现象 | 首先检查 |
| --- | --- |
| 8554 不通 | 当前 IP、热点、程序是否运行及监听 |
| 有视频但没标注 | 是否播放 main_stream，是否禁用 AI/OSD，物体是否属于 11 类 |
| 单图识别正常，实时没框 | AI 日志、目标大小、旋转、摄像头画面与训练数据的差异 |
| 名称错或漏检 | 模型精度、光照、目标尺度；本模型不是成熟成品 |
| 端口占用/摄像头打开失败 | 是否同时启动了旧人脸程序或第二个新程序 |

模型和数据来源、训练命令见 README.md，实际验证版本见 STATUS.md。
