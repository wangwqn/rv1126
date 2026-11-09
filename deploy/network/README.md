# RV1126 确定性 Wi-Fi 冷启动

`rv1126-wifi` 在 OTA 断电事务恢复后、NTP 之前启动。它不把 SSID/PSK 放入 Git
或 OTA，只读取板端0600文件：

```text
/userdata/rv1126-network/wpa_supplicant.conf
/userdata/rv1126-network/network.conf
```

`network.conf` 示例：

```ini
ADDRESS=192.168.137.109/24
GATEWAY=192.168.137.1
DNS1=192.168.137.1
DNS2=223.5.5.5
```

服务直接管理 `wpa_supplicant`，避免精简 Buildroot 中 ConnMan 服务记录未持久化。
热点未开启时保持扫描，NTP 每15秒重试；热点出现后无需重启即可校时并恢复录像。

```bash
/root/rv1126_objects/rv1126-wifi status
/root/rv1126_objects/rv1126-wifi health
```
