# RV1126 安全认证与应用 OTA

## 信任边界

- OTA 私钥只存在离线发布机，权限为0600，不得复制到开发板、
  Git、CI日志或OTA包。
- 设备仅保存RSA公钥JSON，位于
  `/userdata/rv1126-ota/trusted_keys/<key-id>.json`。
- Web管理令牌、云端设备HMAC凭证和TLS服务端私钥是三组独立凭证。
- 第一阶段OTA只能改动应用、RKNN模型和Web资源，不能访问
  boot、kernel、rootfs或任意系统路径。

## 目录

```text
/root/rv1126_objects -> /root/rv1126_releases/v1.11.0
/root/rv1126_releases/v1.10.1
/root/rv1126_releases/v1.11.0
/root/rv1126_ota/rv1126_ota.py
/userdata/rv1126-ota/
  trusted_keys/
  incoming/update.rvota
  persistent/
  active.json
  staged.json
  transaction.json
  history.jsonl
```

`runtime.args`、`runtime-config.json`、`web.token`、事件、截图、录像和
审计日志位于持久化目录，每个发布目录只包含指向它们的软链接。

## 生成离线签名密钥

`KEY_DIR` 必须在仓库外，正式私钥应在断网发布机或HSM生成：

```bash
./scripts/create_ota_signing_key.sh "$KEY_DIR"
```

备份私钥后，只将 `<key-id>.json` 公钥交给设备配置人员。

## 创建签名包

```bash
./scripts/build_ota_package.py \
  --release release/rv1126 \
  --private-key "$KEY_DIR/rv1126-ota-signing-private.pem" \
  --trusted-key "$KEY_DIR/<key-id>.json" \
  --min-version 1.10.1 \
  --output /tmp/rv1126-v1.11.0.rvota
```

## 首次A/B迁移

```bash
./scripts/install_ota_runtime.sh "$KEY_DIR/<key-id>.json"
```

该命令将当前已验证应用移入 `rv1126_releases`，迁移运行时状态，
建立稳定软链接和S97断电恢复服务，然后重启并等待健康检查。

## HTTPS

测试可生成唯一设备自签名证书；产品应改用设备CA签发的证书：

```bash
./scripts/create_device_certificate.sh "$CERT_DIR" rv1126-003 192.168.137.109
./scripts/install_https.sh "$CERT_DIR/rv1126-003.crt" "$CERT_DIR/rv1126-003.key"
```

Nginx对外监听443，Python只监听127.0.0.1:8080。

## API

```text
GET  /api/ota/status
GET  /api/ota/history
POST /api/ota/upload?sha256=<bundle-sha256>
POST /api/ota/verify
POST /api/ota/apply       {"confirm":true}
POST /api/ota/rollback    {"confirm":true}
POST /api/auth/token/rotate      {"confirm":true}
POST /api/auth/token/invalidate  {"confirm":true}
```

连续5次鉴权失败会封禁客户端IP 300秒。令牌失效后只能在本地执行：

```bash
/root/rv1126_objects/rv1126-web token-reset
```

## 事务和回滚

1. 上传阶段只检查整包SHA-256。
2. 验证阶段先读取manifest和签名，验证RSA签名、设备、版本及序号；
   通过后才把声明的普通文件写入备用目录，并逐文件复核SHA-256。
3. 应用阶段先落盘 `transaction.json`，再原子切换软链接。
4. 新版本60秒内必须通过媒体和Web健康检查，否则切回上一版。
5. 切换期间断电时，S97在S98 Web和S99媒体启动前无条件恢复上一版。
