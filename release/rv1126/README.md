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
当前主流默认 6 Mbps、子流默认 600 kbps；6 Mbps 已通过 120 秒主/子双路并发
解码测试，8 Mbps 在板载旧版 `librtsp` 上不稳定。弱 Wi-Fi 可在启动时指定
`--bitrate 5000000` 或 `--bitrate 4000000`。

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
