# RV1126 发布包

这个目录用于在没有本地交叉编译环境时，将已经验证过的程序直接部署到相同平台。

## 兼容范围

- ALIENTEK RV1126 开发板，32 位 ARM Linux。
- 当前项目所用的 RKMedia、RGA、RKNN 1.7、RKAiq 和 glibc/libstdc++ ABI。
- IMX415 摄像头，`rkispp_scale0`/`rkispp_scale1` 视频节点拓扑。
- RK809/RK817 ALSA capture `card 0, device 0` 和 `libasound.so.2`。
- 不保证兼容 RV1126B、其他厂商 rootfs 或不同版本 Rockchip 多媒体库。

`objects/` 是当前23类常见物品检测的完整运行集合。`face/` 是原始 RockX 人脸检测
程序；它依赖开发板 rootfs 中已有的 RockX 模型和运行库。两个程序不能同时占用摄像头
和 TCP 8554 端口。

`candidates/objects23/` 保留23类模型成为正式版本之前的候选验证资料；当前相同模型
已经提升到 `objects/`。逐类精度和长稳回归仍未完成。

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
bash scripts/deploy_release.sh --restart
adb shell '/root/rv1126_objects/rv1126-object status'
adb shell '/root/rv1126_objects/rv1126-object health'
```

部署使用隐藏暂存目录：全部文件上传后先在板端执行 SHA256 校验，通过后
才用同文件系统的 `rename` 替换正式文件，因此中断上传不会把主程序
截断为空文件。同名旧文件保留为 `.previous` 硬链接，且主可执行文件最后替换。
已运行进程不会被部署脚本中断；执行 `rv1126-object restart` 后才切换到新 inode。

其他管理命令：

```bash
adb shell '/root/rv1126_objects/rv1126-object stop'
adb shell '/root/rv1126_objects/rv1126-object restart'
adb shell '/root/rv1126_objects/rv1126-object health'
adb shell 'tail -100 /tmp/rv1126-object.log'
# 可选：安装 Buildroot 开机服务
adb shell '/root/rv1126_objects/rv1126-object install-autostart'
```

`health` 会在采样窗口内确认主/子视频帧和音频包计数增长，并检查 RTSP 监听、
ISPP `ErrCnt` 和 TNR。宿主机可执行 `bash scripts/rollback_device.sh` 恢复上一次
已验证部署；回滚前会先校验全部 `.previous` 文件，失败时不停止当前服务，
交换后会等待板端健康检查通过。

主流地址为 `rtsp://开发板IP:8554/live/main_stream`，子流地址为
`rtsp://开发板IP:8554/live/sub_stream`。
两条 Session 都默认包含 8 kHz 单声道 G.711A/PCMA 音轨。程序从
`plughw:0,0` 采集双声道 PCM、选择 Main Mic、下混并以 20 ms 分包；
`--no-audio` 可关闭。执行 `bash scripts/test_rtsp_av.sh 开发板IP 10` 可同时验收。
当前主流默认4 Mbps、子流默认600 kbps。正式管理脚本默认以 `--sensor-mode 4k`
选择IMX415 3864×2192 RAW10、有效裁剪3840×2160，再输出1080p/360p并整形为
稳定29 FPS，TNR仍由板端IQ配置保持开启。30秒双流各869帧、零错误且服务端
`pts_gap=0`。程序直接调用时的内建传感器默认仍是1080p；旧4K输入在标称30 FPS、
4/5 Mbps时都约为29.5 FPS并累计PTS缺口。1080p传感器模式真机连续
超过 17,000 帧/路仍为 `pts_gap=0`，ADB TCP 双流 60 秒各解码 1800 帧。
30 分钟/24 小时长稳定性验收仍待完成。
板载 IQ 缺少 1080p ALSC 标定，因此该模式明确旁路 ALSC；正式产品应补做镜头标定。
可用 `--sensor-mode 4k` 回退旧模式，当前发布程序没有 `--disable-tnr` 参数。
8 Mbps 在板载旧版 `librtsp` 上仍是已知不稳定配置。

AI专用RGA通道在输入端使用RKMedia帧率控制，只将默认3 FPS送入NV12→BGR转换，
不再以视频帧率转换后由应用丢弃。全视场完整功能真机短测中，应用CPU占四核总容量
由26.30%降至19.65%，29 FPS保持 `pts_gap=0`。测试时SoC已达84°C且热管理介入，
30 FPS仍不稳定；要重新验收30 FPS，必须先改善散热并按同一测试流程复测。

分段时延诊断可在启动命令后添加 `--latency-stats`。程序每 5 秒报告 AI、OSD、
主/子编码输出和 RTSP 发送路径的滚动 P50/P95/P99/max；正常运行默认关闭。
完整指标边界和抓取脚本见 `deploy/pipeline/docs/runtime-latency.md`。

保存逐样本数据时使用 `--latency-csv /tmp/latency.csv`。CSV 由独立线程异步写入；
推荐从 Ubuntu 执行 `bash scripts/capture_runtime_latency.sh 60`，自动拉取 CSV、
检查是否丢诊断记录，并生成全量百分位表和 SVG 图。

1.5.1 的 OSD 更新由 AI 检测结果通过条件变量直接唤醒，不再每 20 ms 轮询；真机
逐样本验证的 OSD 等待 P99 从约 20.0 ms 降至约 0.64 ms，视频 DMA-BUF、VENC
和 RTSP 数据路径未改变。

1.5.2 复用 RKNN 前的 letterbox 图像和候选框工作区，避免 AI 线程每帧重复
申请输入图内存。真机同时段 A/B 中 `ai.infer` P99 从 17.827 ms 降至
17.567 ms；这是软件预/后处理小幅优化，不代表 RKNN/NPU 单独耗时降低。

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
