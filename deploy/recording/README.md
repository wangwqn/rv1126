# RV1126 本地录像

`rv1126-recorder` 从本机 RTSP 主流取得 H.264 + G.711A，不解码、不重编码，按时长
分片封装为 MKV。`rv1126-recording.py` 监控子进程、保留事件前后片段，并按容量与
剩余空间删除最旧录像。录像只保存在开发板，不在云端上报路径中。

默认目录：

```text
/root/rv1126_objects/recordings/continuous/  循环录像
/root/rv1126_objects/recordings/events/      事件前后录像和 event.json
/userdata/rv1126-recording/status.json       健康、容量和重启统计
```

录像目录通过 OTA 的持久化链接跨 A/B 版本保留。Web 中可配置启用状态、分片时长、
循环容量、最低剩余空间、事件前/后秒数和事件保留容量。管理器对“进程存活但
分片无进度”也会自动重启，不只检查 PID。

```bash
/root/rv1126_objects/rv1126-recording status
/root/rv1126_objects/rv1126-recording health
/root/rv1126_objects/rv1126-recording restart
```

事件来自 `events/events.jsonl`。分片选择使用会话起始时间和序号计算，不依赖
Matroska 关闭时可能统一更新的文件 mtime。容量策略总是保护最新两个分片和正在
处理的事件目录。
