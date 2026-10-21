# 实测状态（2026-10-19）

## 当前开发板正在运行的版本

- 目录：`/root/rv1126_objects`，未覆盖原人脸程序或系统运行库。
- 权重来源：`runs/objects11_relu2/weights/best.pt`，12 epoch 的训练基线。
- 模型：`artifacts/objects-rv1126.rknn`，1,899,778 字节。
- SHA256：`661285a3c12e43fd58c2e2018adb1d8da5766bbf43601992b3468854b78402b8`。
- 转换工具：RKNN Toolkit 1.7.5；板端原 runtime 1.7.0 / driver 6.4.6.5.351518 实测可加载。
- ONNX 检查和 PyTorch/ONNX Runtime 数值对比通过。
- C++ NCHW/NHWC 解码、letterbox 还原、按类别 NMS、类别编号缩放、NaN 测试通过。
- 从原管线继承的 portable core_tests 通过；ARM 程序交叉编译通过。

## 开发板实测

公开验证图 `000000046031.jpg`：检出两个鼠标、一个笔记本电脑、一个键盘。
一次预处理 + RKNN 推理 + 后处理耗时 16.42 ms；**不是持续性能基准或 NPU 单独耗时**。
结果图：`artifacts/device-initial-result.jpg`，中文名称和置信度显示正常。

实时流已运行并抓帧：`artifacts/live-initial.png`。
主流 H.264、720×1280、20 fps（旋转 270 度后的参数）；实时画面已有中文标注。
当前场景出现了容器同时被标成杯子/瓶子的误检，尚需补数据和优化精度。

当前 Wi-Fi：`wang`，DHCP 地址 `192.168.137.109`，网关 `192.168.137.1`。
网关 ping 2/2 成功；Ubuntu 直接经 Wi-Fi ffprobe 主码流成功。
后台 PID 文件 `/tmp/rkvision-objects.pid`，日志 `/tmp/rkvision-objects.log`。
这些是当次运行状态，重启/重新分配 DHCP 后请重新确认，不是开机自启配置。

## 后续训练结果（尚未替换板上模型）

额外 36 epoch 微调已结束：`runs/objects11_refine/weights/best.pt`。
该权重的验证集总体 P=0.439、R=0.312、mAP@0.5=0.312、mAP@0.5:0.95=0.175。
原基线 mAP@0.5=0.303；改善有限，书籍和部分水果仍较弱。
为保持用户正在测试的版本稳定，板端仍运行上方已验证的基线 RKNN。
微调权重尚未重新导出、量化及上板验证；不能把微调指标当作当前板端量化精度。

测试流程见 `TESTING.md`。模型是小数据基线，不能据此承诺全部类别稳定识别。

## 2026-10-21 1080P30 优化版真机进展

- IMX415 已确认以 3840×2160、30 FPS 输入 ISP；主流输出 1080×1920，子流输出
  360×640，均为 H.264 Constrained Baseline、名义 30 FPS。
- 视频链路已经改为 `VI DMA-BUF -> RGA NV12 -> VENC` 硬件绑定；AI 使用独立的
  640×360 BGR RGA 分支，不再让 OpenCV 读写整幅 1080P 视频。
- RGA region 已在主流显示绿色框、中文名称和置信度；真机抓帧颜色和旋转正常。
- 8 Mbps 主流会触发板载旧 `librtsp` TCP 拥塞及码流损坏。5 Mbps 双路解码
  60 秒通过；6 Mbps 双路解码 60 秒及 120 秒耐久复测也通过，两路分别完整收到
  1800/3600 帧，无 FFmpeg、H.264 或 RTSP 错误，当前默认值提升为 6 Mbps。
- TNR 改为在 VI/RKAIQ 启动后关闭后，本次 6 Mbps 耐久测试客户端为 30.13 FPS，
  服务端稳态 `pts_gap=0`、队列高水位 2/8、AI `stale_drop=0`。旧版约 29.x FPS
  和每秒一个缺口是修复前结果；短测通过仍不等同于 24 小时绝对零丢帧承诺。
- AI 降到 3 FPS 后，双路客户端负载下应用进程占四核总容量 25.23%，RSS/HWM
  约 119948 kB；满足“应用进程 CPU <30%”口径。整机 busy 当次为 47.85%，
  不能混同为应用 CPU。
- 编码端前台统计约 12 秒主、子各输出 365 帧，RTSP 阻塞队列高水位 2/8；
  `/proc/<pid>/fd` 可见 DMA-BUF 句柄。
- 30 分钟/24 小时稳定性和计时器光学对拍仍待执行；在完成 P50/P95/P99 对拍前，
  不宣称端到端低于 200 ms，也不把短测结果表述为长期绝对零丢帧。
- 当前板端程序 SHA256：
  `720878d078fab503580914c9a48738be28eeb182f7b8ac83ca79546803357267`。

完整验收命令和口径见 `deploy/pipeline/docs/board-validation.md`。
