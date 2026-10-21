# RV1126 发布包

这个目录用于在没有本地交叉编译环境时，将已经验证过的程序直接部署到相同平台。

## 兼容范围

- ALIENTEK RV1126 开发板，32 位 ARM Linux。
- 当前项目所用的 RKMedia、RGA、RKNN 1.7、RKAiq 和 glibc/libstdc++ ABI。
- IMX415 摄像头，`rkispp_scale0`/`rkispp_scale1` 视频节点拓扑。
- 不保证兼容 RV1126B、其他厂商 rootfs 或不同版本 Rockchip 多媒体库。

`objects/` 是 11 类常见物品检测的完整运行集合。`face/` 是原始 RockX 人脸检测
程序；它依赖开发板 rootfs 中已有的 RockX 模型和运行库。两个程序不能同时占用摄像头
和 TCP 8554 端口。

## 校验

在仓库根目录执行：

```bash
bash scripts/verify_checkout.sh
```

或直接执行：

```bash
cd release/rv1126
sha256sum -c SHA256SUMS
```

## 部署物体检测

```bash
bash scripts/deploy_release.sh
adb shell
cd /root/rv1126_objects
./rkmedia_object_detection_rtsp --rotation 270 --threshold 0.35
```

主流地址为 `rtsp://开发板IP:8554/live/main_stream`，子流地址为
`rtsp://开发板IP:8554/live/sub_stream`。
当前主流默认 4 Mbps、子流默认 600 kbps。程序默认以 `--sensor-mode 1080p` 选择
IMX415 1944×1097 RAW12、有效裁剪 1920×1080、30 FPS 模式，TNR 仍由板端 IQ
配置保持开启。旧 4K 输入在 4/5 Mbps 时都约为 29.5 FPS；1080p 候选版约 10 秒
短测为主流 299 帧、子流 300 帧且稳态 `pts_gap=0`，长时间验收仍待完成。
板载 IQ 缺少 1080p ALSC 标定，因此该模式明确旁路 ALSC；正式产品应补做镜头标定。
可用 `--sensor-mode 4k` 回退旧模式，当前发布程序没有 `--disable-tnr` 参数。
8 Mbps 在板载旧版 `librtsp` 上仍是已知不稳定配置。

局域网播放默认选择 RTP/UDP，RTSP 控制连接仍使用 TCP 8554。FFplay 示例：

```bash
ffplay -rtsp_transport udp -buffer_size 2097152 \
  -reorder_queue_size 512 -max_delay 200000 \
  rtsp://开发板IP:8554/live/main_stream
```

ADB forward 不支持 UDP 媒体端口。UDP 被防火墙或网络策略阻断时，再回退到
`-rtsp_transport tcp`。

如二进制提示缺少共享库、非法指令或摄像头节点不存在，请不要替换板载系统库，应使用
`deploy/pipeline/cmake/toolchains/rv1126.cmake` 和目标板对应 SDK 重新交叉编译。
