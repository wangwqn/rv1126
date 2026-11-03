# RV1126 断电后自动校时

`rv1126-time-sync.py` 使用 Python 标准库实现 SNTP，不依赖根文件系统中可能缺失的
`ntpd`/`chronyd`。开机脚本 `S97zrv1126-time-sync` 会在 OTA 断电事务恢复后、
媒体和录像服务前启动守护进程。

默认同时查询阿里云、腾讯云、中国 NTP pool 和 Cloudflare，对有效样本取中位数，
拒绝超出共识窗口的结果。校时成功后同步写 RTC；网络未就绪时每 15 秒重试，
成功后每 6 小时复核。状态保存在 `/userdata/rv1126-time/status.json`。

```bash
/root/rv1126_objects/rv1126-time-sync status
/root/rv1126_objects/rv1126-time-sync health
/root/rv1126_objects/rv1126-time-sync once
```

NTP 使用 UDP/123，热点或防火墙必须允许出站访问。校时失败不会阻止视频启动，
但录像管理器会在系统时间早于 2024 年时拒绝创建错误时间戳的录像。
