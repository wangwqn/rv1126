# RV1126 v1.9.3 看门狗与 Wi-Fi 短时验收（2026-10-27）

## 环境

- 开发板：ATK-DLRV1126，IMX415，RKISPP TNR 2to1。
- 程序版本：1.9.3，Git 提交待本次验收提交固化。
- 媒体配置：1920x1080 + 640x360，30 FPS，AI 3 FPS，
  4 Mbit/s + 600 kbit/s，OSD TTL 500 ms，PCMA 8 kHz。
- Wi-Fi：Windows 热点 `wang`，板端 `192.168.137.109`；RTSP 使用
  真实 Wi-Fi IP，未使用 ADB 端口转发。

## 现场发现与修正

1. v1.9.1 使用 `nohup ... &` 启动普通后台 shell，ADB 会话
   结束后看门狗被回收。
2. v1.9.2 将看门狗改为 BusyBox `start-stop-daemon -b -m`，
   确认可跨 ADB 会话存活；启动宽限期从15秒调整为30秒。
3. 断电启动时 `rcS` 不加载 `/etc/profile.d/RkEnv.sh`，最小复现为
   `libeasymedia.so.1` 无法解析。v1.9.3 在管理脚本内显式导出
   OEM `PATH`/`LD_LIBRARY_PATH`，并保留首次失败后看门狗重试。
4. 用与 `rcS` 等价的 `env -i` 空环境启动 v1.9.3，45秒后
   `health` 通过，看门狗 PID 持续存活，重启计数为0。

## 故障注入

- `SIGKILL` 崩溃：旧 PID 3035，新 PID 327；看门狗约10秒确认
  连续失败，约25秒内完整恢复健康。
- `SIGSTOP` 卡死：旧 PID 327，新 PID 1129；先检出 VENC 输出
  停滞，继而检出 RKISPP 停滞，TERM 8秒无效后 KILL，约40秒
  恢复为完整健康。
- 两次恢复后均为 AI/OSD/AUDIO ON、ErrCnt=0、TNR ON、RTSP 8554。

## Wi-Fi RTSP 与性能

- 60秒 TCP 双流：主流1801帧、子流1801帧，均30.02 FPS，PASS；
  无 H.264/RTP/解码错误。
- 30秒音视频：主子流均为 H.264 + 8 kHz 单声道 PCMA；
  243520个音频样本，平均 -37.9 dB、峰值 -17.3 dB，PASS。
- 双流负载下20秒 CPU 样本：应用占四核总容量14.34%。
- RSS/HWM 119400 kB，34线程，160文件描述符。
- CPU/NPU 温度60.6/58.7摄氏度。
- 同一20秒内主子 VENC 各增长601帧，AI完成增长60次，
  音频增长1002包；`pts_gap=0`、`stale_drop=0`、`queue_drop=0`。
- `env -i` 启动后10秒 Wi-Fi 复测：主流300帧、子流299帧，PASS。

## 尚未完成

- v1.9.3 修正后的再次物理断电冷启动。
- 持续30分钟完整功能验收和24小时稳定性验收。
- 画面中未放置已训练目标，本次日志 `osd(boxes=0)`，因此
  OSD 500 ms TTL 需用持续目标录像补充视觉验收。
