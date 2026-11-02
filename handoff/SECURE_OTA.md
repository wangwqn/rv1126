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
/root/rv1126_objects -> /root/rv1126_releases/v1.11.1
/root/rv1126_releases/v1.10.1
/root/rv1126_releases/v1.11.0
/root/rv1126_releases/v1.11.1
/root/rv1126_ota/rv1126_ota.py
/userdata/rv1126-ota/
  trusted_keys/
  incoming/update.rvota
  persistent/
  active.json
  staged.json
  transaction.json
  history.jsonl
  revocations.json
  policy.json
  failure-state.json
  release-state.json
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
  --channel stable \
  --output /tmp/rv1126-v1.13.0.rvota
```

schema 2清单还包含 `published_at`、`channel`、`component`、`package_type`、
`base_version`和依赖最低版本。默认设备通道是 `stable`：stable只接收stable，
beta接收stable/beta，development接收全部通道。板端本地切换：

```bash
/usr/bin/python3 /root/rv1126_ota/rv1126_ota.py set-channel beta
```

独立模型包或Web包采用overlay：发布机必须提供与板端当前版本相同的已验证基础快照。
打包器只上传变化文件，板端先克隆当前已验证版本，再覆盖签名文件并整体A/B切换：

```bash
./scripts/build_ota_package.py \
  --release release/rv1126 \
  --base-release /secure/releases/v1.12.2 \
  --component model \
  --channel beta \
  --private-key "$KEY_DIR/rv1126-ota-signing-private.pem" \
  --trusted-key "$KEY_DIR/<key-id>.json" \
  --min-version 1.12.2 \
  --output /tmp/rv1126-model-v1.13.0.rvota
```

`--component`支持 `full/application/model/web`。这是文件级差分，不依赖板端
`bspatch`，不会对未签名基线执行二进制补丁；模型本体变化时仍需上传完整RKNN文件。
可用 `--dependency NAME=MIN_VERSION` 增加依赖，板端库存记录在
`/userdata/rv1126-ota/dependencies.json`。

## 公钥轮换和吊销

先在板端安装新公钥，用新私钥签一个beta包完成验收，再吊销旧密钥：

```bash
OTA=/root/rv1126_ota/rv1126_ota.py
/usr/bin/python3 "$OTA" key-install /tmp/new-public-key.json
/usr/bin/python3 "$OTA" key-revoke OLD_KEY_ID --reason "rotation completed"
```

吊销表位于持久化目录，验签和应用阶段都会检查。紧急误操作只能在板端root控制台
执行 `key-unrevoke KEY_ID`；Web不提供密钥吊销或解锁接口，避免远程令牌被盗后修改
信任根。

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

连续3次应用失败后写入 `failure-state.json` 并锁定后续apply，同时状态接口暴露告警。
排除根因后只能在板端执行：

```bash
/usr/bin/python3 /root/rv1126_ota/rv1126_ota.py unlock --confirm
```

保留策略只自动删除明确标记为 `verified` 的旧版本，并按 `verified_at`排序；当前版、
上一已验证版和未知旧目录不会因mtime被误删。最近一个失败候选会保留用于取证。

上传默认限速4 MiB/s，暂存写入默认限速8 MiB/s；同时检查剩余空间和每CPU负载。
策略保存在 `/userdata/rv1126-ota/policy.json`，默认保留至少64 MiB空闲空间。

## 物理断电验收

该测试会在候选软链接已经切换、事务状态为 `validating`且服务尚未启动时暂停120秒。
必须准备串口/Loader恢复手段，并确认上一版本健康后执行：

```bash
bash scripts/test_ota_power_loss.sh arm /tmp/candidate.rvota \
  --confirm-physical-power-test
# 终端显示 CUT POWER NOW 后拔电，等待至少10秒，再上电
bash scripts/test_ota_power_loss.sh verify
```

通过标准是事务为 `power_loss_rolled_back`、软链接恢复上一已验证版本，媒体和Web健康。
脚本使用一次性root标记；普通OTA无法进入断电测试暂停点。
