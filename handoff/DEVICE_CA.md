# 设备 HTTPS CA

私有 IP 不能由 Let's Encrypt 等公共 CA 签发。本项目使用一个离线 RSA-3072 设备 CA，
对每块板的 IP/主机名签发 `serverAuth` 证书。浏览器导入公开根证书后，即可
校验设备身份，不再使用自签名服务器证书。

离线 CA 私钥和设备私钥必须保存在 Git 仓库外。开发板只需设备证书与对应
设备私钥；Windows 只需公开 CA 证书，绝不复制 CA 私钥。

```bash
scripts/create_device_ca.sh /outside/repository/device-ca "RV1126 Local Device Root CA"
scripts/issue_device_ca_certificate.sh \
  /outside/repository/device-ca/rv1126-device-ca.crt \
  /outside/repository/device-ca/rv1126-device-ca.key \
  /outside/repository/rv1126-003 rv1126-003 192.168.137.5 rv1126-003.local
scripts/install_https.sh \
  /outside/repository/rv1126-003/rv1126-003.crt \
  /outside/repository/rv1126-003/rv1126-003.key \
  /outside/repository/device-ca/rv1126-device-ca.crt
```

Windows 管理员 PowerShell：

```powershell
certutil -addstore -f Root .\rv1126-device-ca.crt
```

导入后完全退出并重新打开浏览器，访问证书 SAN 中的 IP 或主机名。只有在
`openssl verify` 成功且浏览器已导入根证书后才能称为“可信”；`curl -k` 不是验收。
