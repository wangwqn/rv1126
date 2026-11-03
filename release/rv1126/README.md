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

## 首次部署物体检测

```bash
bash scripts/deploy_release.sh --restart
adb shell '/root/rv1126_objects/rv1126-object status'
adb shell '/root/rv1126_objects/rv1126-object health'
adb shell '/root/rv1126_objects/rv1126-web install-autostart'
adb shell '/root/rv1126_objects/rv1126-web start'
adb shell '/root/rv1126_objects/rv1126-web token'
```

上述ADB命令仅用于尚未迁移A/B布局的首次安装。产品模式必须按
`handoff/SECURE_OTA.md` 配置离线签名公钥、A/B目录和Nginx HTTPS。
完成后同一Wi-Fi下访问 `https://开发板IP/`，使用
`rv1126-web token` 输出的Bearer Token登录。Python仅监听
`127.0.0.1:8080`，对外HTTPS由Nginx提供；Web不进入DMA-BUF/编码/RTSP数据路径。

界面支持主/子码率、GOP、AI FPS、置信度、OSD TTL/数量与客户端
缓冲配置。勾选“立即应用”会中断 RTSP 并重启管线；若健康检查
失败，服务会恢复旧配置并尝试重启。区域/越线当前仅保存坐标，
跟踪和规则事件引擎是后续扩展。

完成A/B迁移后，`deploy_release.sh` 会拒绝原地覆盖。后续发布必须使用
RSA-3072/SHA-256签名 `.rvota` 包：先完整写入备用目录并复核摘要，
再原子切换 `/root/rv1126_objects` 软链接；60秒健康门禁失败会自动回滚。

其他管理命令：

```bash
adb shell '/root/rv1126_objects/rv1126-object stop'
adb shell '/root/rv1126_objects/rv1126-object restart'
adb shell '/root/rv1126_objects/rv1126-object health'
adb shell '/root/rv1126_objects/rv1126-object watchdog-status'
adb shell '/root/rv1126_objects/rv1126-object watchdog-log'
adb shell 'tail -100 /tmp/rv1126-object.log'
# 可选：安装 Buildroot 开机服务
adb shell '/root/rv1126_objects/rv1126-object install-autostart'
```

`health` 会在采样窗口内确认主/子视频帧和音频包计数增长，并检查 RTSP 监听、
ISPP `ErrCnt`、TNR和外部看门狗，同时拒绝以 `--no-ai`、`--no-osd` 或
`--no-audio` 启动的进程。看门狗默认每5秒检查进程、8554、RKISPP和
VENC计数，连续2次异常后自动重启；日志保存在
`/tmp/rv1126-object-watchdog.log`。宿主机可执行 `bash scripts/rollback_device.sh` 恢复上一次
已验证部署；回滚前会先校验全部 `.previous` 文件，失败时不停止当前服务，
交换后会等待板端健康检查通过。
默认 OSD 结果 TTL 为500 ms，为3 FPS AI更新留出调度余量；
空检测结果仍立即清框。

主流地址为 `rtsp://开发板IP:8554/live/main_stream`，子流地址为
`rtsp://开发板IP:8554/live/sub_stream`。
两条 Session 都默认包含 8 kHz 单声道 G.711A/PCMA 音轨。程序从
`plughw:0,0` 采集双声道 PCM、选择 Main Mic、下混并以 20 ms 分包；
`--no-audio` 可关闭。执行 `bash scripts/test_rtsp_av.sh 开发板IP 10` 可同时验收。
当前主流默认4 Mbps、子流默认600 kbps。正式管理脚本默认选择
IMX415 1944×1097 RAW12 2×2 binning，对齐1920×1080有效区域，输出
1080p/360p@30；TNR 2to1、NR、SHARP、AI、OSD和音频默认开启。
旧4K RAW10输入可用 `--sensor-mode 4k` 回退，但实测约29.x FPS并累计PTS缺口。
1080p binning 模式真机连续超过17,000帧/路仍为 `pts_gap=0`，ADB TCP双流
60秒各解码1800帧；v1.8.8全功能无参数启动后再次实测3秒各墑长约92帧。
30 分钟/24 小时长稳定性验收仍待完成。
板载 IQ 缺少 1080p ALSC 标定，因此该模式明确旁路 ALSC；正式产品应补做镜头标定。
当前发布程序没有 `--disable-tnr` 参数。OSD默认按置信度显示最多8个
中文名称+置信度检测框，可用 `--max-osd-boxes 1..8` 调整。

1.15.0 新增开机 SNTP/RTC 校时和本地录像：

```bash
/root/rv1126_objects/rv1126-time-sync health
/root/rv1126_objects/rv1126-recording health
```

录像按默认10秒分片保存 H.264 + G.711A MKV，支持循环覆盖和区域/越线事件前后
录像；录像仅保存在板端。详细策略见 `deploy/recording/README.md`。HTTPS 生产环境
应使用设备 CA 签名证书，Windows 只导入公开根证书，见 `handoff/DEVICE_CA.md`。
v1.9.4 起使用项目自有的异步 RTP-over-TCP 服务端替换旧 `librtsp`；当前发布仍以
4 Mbps 作为默认值。RTSP URL 不变，客户端需使用 TCP transport。v1.9.6 增加
`--rtsp-client-buffer-kb 128..4096`，默认512 KiB；扩大缓冲能吸收短时拥塞，
但会提高拥塞期间的瞬时延迟。

AI专用RGA通道在输入端使用RKMedia帧率控制，只将默认3 FPS送入NV12→BGR转换，
不再以视频帧率转换后由应用丢弃。全视场完整功能真机短测中，应用CPU占四核总容量
由26.30%降至19.65%，29 FPS保持 `pts_gap=0`。测试时SoC已达84°C且热管理介入，
30 FPS仍不稳定；要重新验收30 FPS，必须先改善散热并按同一测试流程复测。

分段时延诊断可在启动命令后添加 `--latency-stats`。程序每 5 秒报告 AI、OSD、
主/子编码输出和 RTSP 发送路径的滚动 P50/P95/P99/max；正常运行默认关闭。
完整指标边界和抓取脚本见 `deploy/pipeline/docs/runtime-latency.md`。

1.12.0默认追加 `--dma-audit`，每5秒报告 `MEDIA_BUFFER` 获取/释放平衡、
DMA FD、逾期持有、CPU cache-sync映射、进程/内核DMA-BUF以及获取和持有
P50/P95/P99。主/子NV12零拷贝的关键判定是 `cpu(nv12=0)`且无
`WARNING=buffer_lifecycle`。详细边界见 `deploy/pipeline/docs/dma-buffer-audit.md`，
长时采集使用 `bash scripts/capture_dmabuf_audit.sh 1800`。

保存逐样本数据时使用 `--latency-csv /tmp/latency.csv`。CSV 由独立线程异步写入；
推荐从 Ubuntu 执行 `bash scripts/capture_runtime_latency.sh 60`，自动拉取 CSV、
检查是否丢诊断记录，并生成全量百分位表和 SVG 图。

1.5.1 的 OSD 更新由 AI 检测结果通过条件变量直接唤醒，不再每 20 ms 轮询；真机
逐样本验证的 OSD 等待 P99 从约 20.0 ms 降至约 0.64 ms，视频 DMA-BUF、VENC
和 RTSP 数据路径未改变。

1.5.2 复用 RKNN 前的 letterbox 图像和候选框工作区，避免 AI 线程每帧重复
申请输入图内存。真机同时段 A/B 中 `ai.infer` P99 从 17.827 ms 降至
17.567 ms；这是软件预/后处理小幅优化，不代表 RKNN/NPU 单独耗时降低。

当前自建服务端只支持RTP-over-TCP interleaved。FFplay示例：

```bash
ffplay -rtsp_transport tcp -fflags nobuffer -flags low_delay \
  -analyzeduration 100000 -probesize 32768 \
  rtsp://开发板IP:8554/live/main_stream
```

如二进制提示缺少共享库、非法指令或摄像头节点不存在，请不要替换板载系统库，应使用
`deploy/pipeline/cmake/toolchains/rv1126.cmake` 和目标板对应 SDK 重新交叉编译。
