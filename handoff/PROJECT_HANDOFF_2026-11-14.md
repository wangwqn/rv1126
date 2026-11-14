# RV1126视觉项目完整交接档案

更新时间：2026-11-14，北京时间。最新板端只读核对：21:49～21:50。

用途：供另一位工程师或不同AI快速接手开发、学习、诊断和验收。本文件依据当前源码、Git、板端读取和已保存报告整理；包含历史结论的纠正，不是逐字聊天导出。

## 0. 先读这一页：当前事实与禁止误判

1. 硬件为正点原子ATK-DLRV1126、原始RV1126，**不是RV1126B**。摄像头IMX415。
2. 正式板端为`v1.15.3`，发布提交`54b0e0f`。最新代码基线`df92094`增加了拥塞诊断，但这部分未发布到正式板端。后续纯文档提交不代表板端已升级。
3. 当前实际运行的是**23类物品检测**，使用第一代RKNN上的YOLOv5n+ReLU。原RockX人脸工程仍保留，但不是当前运行服务。
4. 当前稳定前端为IMX415 **1944×1097 RAW12、2×2 binning**，有效区域1920×1080；TNR处理1080p。不要描述成当前仍使用4K RAW10的独立离线RKISPP链路。
5. 主流编码前1920×1080，旋转270度后RTSP为1080×1920；子流为360×640。配置30 FPS不等于所有网络客户端始终收到30 FPS。
6. 当前RTSP为项目自建异步**RTP-over-TCP interleaved**，8554端口。历史旧librtsp/UDP方案不是当前后端。
7. AI、OSD、音频、TNR和本地循环录像均开启。开机自启、Wi-Fi恢复、NTP和正常断电冷启已有真机证据。
8. **三小时Wi-Fi严格验收未通过，24小时正式验收尚未完成，没有通过记录。** 不能把短测、服务Healthy或编码端`pts_gap=0`写成端到端零丢帧。
9. 已复现两类独立背压：外部TCP网络传输损失，以及本地录像接收管线偶发停读。两者尚未修复；不能把全部故障只归因于Wi-Fi或录像。
10. 没有完成光学端到端时延和长期音画漂移定量验收；不能宣称端到端200 ms或24小时音画无漂移。
11. `deploy/cloud/`目前是**未跟踪草稿**，未纳入正式发布，不能声称阿里云已上线。不要用`git add .`误收云端草稿或敏感配置。
12. 板端已采用签名A/B应用OTA。正常升级走签名`.rvota`，不要原地覆盖运行版本，也不要因诊断RTSP而刷boot/kernel/rootfs。

文档冲突时，优先采用带时间的真机原始证据、当前实际代码及本档案；`STATUS.md`旧日期章节、旧交接文件、README中的历史IP和早期候选描述需要按时间理解。

## 1. 项目目标与边界

这是一个从嵌入式C++学习项目逐步演进的边缘视觉设备工程，目标包括：

- IMX415采集、ISP/RKISPP图像处理、主子双路H.264编码和RTSP输出。
- 低开销AI检测、多目标中文画框、主子/AI分支解耦。
- ALSA采集、G.711A音频、音视频时间戳和长期同步。
- DMA-BUF缓冲管理、RAII、并发、背压、性能与时延监控。
- Web设备管理、事件、录像、安全认证、签名OTA、故障恢复。
- 自训练模型的PyTorch→ONNX→RKNN→板载部署流程。
- 后续将遥测、事件、日志、截图上传阿里云；录像保留本地。

用户长期性能目标是1080p@30 FPS、尽量零丢帧、CPU低于约30%、端到端约200 ms。它们是待按明确口径验收的目标，不是全部已经达到的指标。

## 2. 工作区、版本与最新运行快照

### 2.1 主机和仓库

| 项目 | 值 |
|---|---|
| 本机仓库 | `/home/alientek/桌面/rv1126_object_training` |
| 工作分支 | `feature/full-fov-pre-tnr-1080` |
| GitHub | `git@github.com:wangwqn/rv1126.git`，remote `origin` |
| Gitee | `git@gitee.com:wang-qiangming/rv1126_ai.git`，remote `gitee` |
| 最新代码基线 | `df92094`：客户端身份和TCP状态诊断，以及板载报告 |
| 最近证据提交 | `e93847c`：v1.15.3真实冷启动报告 |
| 最新正式标签 | `v1.15.3`→`54b0e0f` |
| 上一正式版本 | v1.15.2、v1.15.1、v1.15.0；更早v1.14.1等 |
| 未跟踪内容 | `deploy/cloud/`，应保留并单独审查 |

Git标签对应完整工程快照，推送只传输差异不代表工程不完整。仓库HEAD、`VERSION`、预编译release目录、交叉编译输出、板端进程可以不同，交接时必须分别核对。

在另一台电脑获取本次最新交接资料，明确克隆工作分支：

```bash
git clone --branch feature/full-fov-pre-tnr-1080 git@github.com:wangwqn/rv1126.git
cd rv1126
git status --short
git log -3 --oneline
```

已有克隆时先检查本地修改，再fetch并切换该分支。检出旧正式标签v1.15.3可以恢复当时完整发布，但标签本身不会包含之后新写的本档案。

### 2.2 2026-11-14 21:49板端快照

| 项目 | 读取结果 |
|---|---|
| 正式软链接 | `/root/rv1126_objects`→`/root/rv1126_releases/v1.15.3` |
| VERSION / GIT_COMMIT | `1.15.3` / `54b0e0f` |
| 新启动ID | `60e32228-90f1-4258-980d-9751cc7fbe5a` |
| 初次采样uptime | 约55秒 |
| 媒体PID / 看门狗PID | 915 / 967，重启计数0 |
| 录像进程 | PID 1000，健康，重启计数0 |
| Wi-Fi | SSID `iPhone`，状态COMPLETED |
| 当前地址 | `172.20.10.10/28`，网关`172.20.10.1` |
| 健康 | 双VENC、AI、OSD、音频、TNR开启，ErrCnt=0；NTP健康 |
| 内核 / RKISPP | Linux 4.19.111 / v01.06.02 |
| RKISPP | FBC420 1920×1080输入；scale0 1920×1080；scale1 640×360；约33 ms节拍 |

用户在文档整理中通知开发板已开机，本次读到新boot_id并确认自动恢复；它不是新做的正式长稳验收，也不能仅凭boot_id推断一定经历物理拔电。

历史热点有`wang`、`wang1`，历史地址包括192.168.137.109、.56、.176、.243和10.203.30.11。地址不是设备身份常量。后续换热点必须重新核对地址、掩码、网关和DNS。

### 2.3 当前有效参数与默认值差异

| 参数 | 当前服务值 |
|---|---:|
| sensor-mode | `1080p` |
| 主/子硬件尺寸 | 1920×1080 / 640×360 |
| rotation | 270 |
| fps / GOP | 30 / 30 |
| 主/子码率 | 4,000,000 / 600,000 bit/s |
| AI FPS | 5 |
| 置信度 | 0.25 |
| 结果TTL | 500 ms |
| OSD最大框 | 8 |
| 硬件池配置 | 每路8个；精确实时占用率SDK不公开 |
| 编码包队列 | 8 |
| 每客户端RTSP队列 | 512 KiB |
| 音频 | `plughw:0,0`、main mic、20 ms分包、PCMA 8 kHz单声道 |

`config.hpp`的类默认AI FPS=3、阈值0.35等不一定等于板端当前值；以实际进程参数、`runtime.args`和`runtime-config.json`为准。JSON部分配置缺省时还会使用对应模块默认值。

## 3. 硬件、BSP与开发依赖

- 板卡：ALIENTEK ATK-DLRV1126，ARMv7、32位hard-float；实测可在线4核。
- 系统：Rockchip Linux 4.19.111、Buildroot根文件系统。RKISP/RKISPP为内建驱动。
- 摄像头接口为MIPI CSI-2；RAW10/RAW12是像素格式，两者不是互斥概念。
- 多媒体应用使用RKMedia/easymedia；底层通过V4L2/media controller和DMA-BUF工作，不是应用每路都手写V4L2取帧循环。
- 硬件模块：ISP图像处理，RKISPP TNR/NR/SHARP，RGA旋转/格式转换/区域叠加，VENC编码，NPU推理。
- 当前RKISPP驱动v1.6.2；板载GStreamer 1.14.4，不支持本项目曾尝试使用的`splitmuxsink async-finalize`属性。
- 音频依赖RK809/RK817平台ALSA capture，运行库`libasound`。
- RKNN转换为**第一代Toolkit 1.7.5**，板端Runtime **1.7.0**；该组合对当前模型已有实测加载证据。版本号不同不等于必然不兼容；新模型仍要重新实测。
- 原始RV1126不可直接套用RV1126B、RK3588或Toolkit2部署示例。现代模型算子是否可转换、NPU是否高效均须验证。
- 主机历史环境：Ubuntu 20.04、Python 3.8，训练使用CPU。训练和旧RKNN转换依赖分别放在`.venv-train`、`.venv-rknn`，不更换系统Python。
- 当前媒体工程使用C++14、CMake和Ninja；不要从原工程或旧示例推断成另一C++标准。
- SDK默认`/home/alientek/rv1126_sdk`；工具链GCC8.3、`arm-linux-gnueabihf`。完整SDK和sysroot不在Git中。

## 4. 当前端到端架构

```text
IMX415：2×2 binning，1944×1097 RAW12
  -> 有效记录区域1920×1080（selection left=12, top=8）
  -> RKISP，FBC420 1920×1080
  -> RKISPP：TNR 2to1 + NR + SHARP，均处理1080p
      |
      +-> scale0 1920×1080 NV12
      |    -> VI0 -> RGA0：旋转270°、主流OSD -> VENC0 H.264
      |
      +-> scale1 640×360 NV12
           +-> VI1 -> RGA1：旋转270° -> VENC1 H.264
           +-> RGA2：输入端限速5 FPS、旋转、packed RGB物理数据
                -> CPU同步访问 -> letterbox 320×320 -> RKNN
                -> raw-head解码/NMS -> 检测缓存
                -> OSD更新 / 可选区域越线事件

ALSA PCM S16LE 8 kHz双声道
  -> 下混单声道 -> G.711 A-law -> 音频PTS -> 有界音频队列

VENC0/VENC1编码包 + PCMA音频
  -> 应用RTSP投递队列 -> 异步RTSP worker
      +-> 外部TCP客户端：VLC / FFmpeg / ffprobe
      +-> 本地127.0.0.1主流：GStreamer录像子进程
            -> H.264/PCMA重封装MKV -> 分片/循环删除/事件保留

浏览器 -> Nginx HTTPS443 -> Python Web 127.0.0.1:8080
         -> 状态、配置、文件、事件、模型、OTA、日志
```

“主流、子流、AI流”不是三只摄像头。主流用于较清晰的播放与叠加；子流用于低分辨率预览；AI是独立低频图像分支，通常不对外提供第三个RTSP URL。

主流硬件编码输出为旋转后1080×1920，子流为360×640。看到竖屏尺寸不表示丢失了1080p像素数；它来自当前安装方向与旋转配置。

## 5. 当前功能与验证程度

| 功能 | 实现状态 | 验证与限制 |
|---|---|---|
| IMX415 binning、TNR/NR/SHARP | 已部署 | 1080p硬件节拍及短测30 FPS通过；现场画质仍需IQ标定 |
| 主子H.264双流 | 已部署 | 短测可30 FPS；三小时外部客户端帧数不足，严格长稳未通过 |
| 多目标检测 | 已部署23类 | 多框/标签有真机图像证据；没有生产精度验收 |
| 中文名、置信度、绿色框 | 已部署 | 8个RGA区域合计最多显示8目标；更多目标可能被显示策略截断 |
| OSD TTL防空窗 | 已部署500 ms | 已做逻辑/回归修正；真实目标长时防闪烁录像仍待正式验收 |
| 音频PCMA | 已部署 | 双Session可解码、有声音；不是AAC，也不是长期同步已通过 |
| 区域/越线/轻量跟踪 | 已实现 | Web保存规则、重启媒体加载；当前regions/lines为空，事件引擎未在当前场景启用 |
| 自动连续录像 | 已部署 | 10秒MKV分片、循环覆盖；诊断捕获过恢复录像后的停读与自动重启 |
| 事件前后录像 | 已部署 | 合成事件与固定窗口/循环覆盖验证通过；真实规则场景仍需验收 |
| Web状态与配置 | 已部署 | HTTPS/Bearer、状态和多类API；部分改参需重启媒体，非全部无缝热更新 |
| Web模型管理 | 已实现 | 上传SHA校验、激活和失败恢复；独立模型入口与签名OTA统一策略还应审查 |
| 外部看门狗 | 已部署 | 进程/端口/ISPP/VENC停滞恢复已有故障注入证据 |
| NTP/RTC | 已部署 | 自动校时、失网重试、写RTC；RTC掉电可靠性依赖硬件供电 |
| Wi-Fi开机恢复 | 已部署 | wpa_supplicant持久配置及冷启通过；当前为静态网络配置，不是完整DHCP迁移管理器 |
| HTTPS与设备CA | 已部署 | 对历史签发IP有CA验证通过证据；更换IP须核对SAN |
| RSA签名A/B OTA | 已部署 | 正常升级、重放拒绝、故障回滚有真机证据；OTA切换中物理断电专项未正式通过 |
| 公钥轮换、通道、组件overlay等 | 已实现 | 有主机测试；每个高级策略的真机矩阵不能一概宣称全部完成 |
| DMA/时延观测 | 已实现 | 日志、API、CSV和绘图工具；并非所有硬件内部时延或池占用都可测 |
| 阿里云上报 | 草稿 | 未跟踪、未正式部署；遥控命令未开放 |

### 5.1 当前23类

ID 0～10为手机、杯子、瓶子、书籍、鼠标、键盘、笔记本电脑、显示器/电视、香蕉、苹果、橙子。

ID 11～22依次为人、背包、手提包、碗、三明治、椅子、沙发、盆栽、餐桌、遥控器、时钟、剪刀。

“电脑”目前指笔记本电脑，不是台式机机箱；“水果”只有现有标签中的三类。添加名字、修改JSON不能使模型学会新类别。

### 5.2 区域、越线与事件

- 配置为旋转后画面的归一化坐标，避免将原始横屏坐标直接用于竖屏。
- 规则文件`analytics.rules`，异步事件日志`events/events.jsonl`。
- 实现区域进入/离开、双方向越线、轨迹ID、同类别中心点匹配及去抖。
- 不是ReID，不保证密集遮挡后的身份一致，也未实现跨摄像头身份识别。
- 当前板端规则为空，日志`events=off`是配置状态，不等于引擎源码不存在。
- 配置规则后按当前实现重启媒体生效；需要真实目标移动完成进入、越线和离开验收。

### 5.3 录像

- 默认连续录像512 MiB、最低剩余空间128 MiB、10秒分片。
- 默认事件前录10秒、后录20秒，事件保留192 MiB。
- MKV中是原H.264和PCMA；不解码、不重新编码视频。
- 循环目录和事件目录分开管理，事件采用硬链接保留相交分片。
- 前后窗口按会话文件名时间+分片序号估算，保护窗口会与完整分片边界相交；不是帧级精准剪辑。
- 管理器检查文件进度，不只看PID；长时间无进度会重启子进程。
- latest_segment_age很小只能说明近期文件在更新，不能替代逐片解码或精确音画同步验收。

## 6. 进程、线程、锁与缓冲所有权

### 6.1 进程边界

| 进程/控制器 | 职责 |
|---|---|
| `rkmedia_object_detection_rtsp` | C++媒体、AI、OSD、音频、RTSP |
| `rv1126-object watchdog` | 独立shell看门狗，应用全卡死时仍可恢复 |
| `rv1126-web.py` | Python管理服务，调用固定控制动作，非视频帧搬运路径 |
| Nginx | TLS终结和反向代理 |
| `rv1126-recording.py` | 录像监控、事件保留、循环清理 |
| `rv1126-recorder` | GStreamer拉本地RTSP并重封装MKV |
| `rv1126-time-sync.py` | SNTP、RTC和重试 |
| wpa_supplicant | 无线认证和关联 |
| OTA运行时 | 验签、备用版本构造、切换、健康检查与回滚 |

ALSA线程到PCMA、RTSP的通信发生在同一个媒体进程内；录像与媒体之间是本机RTSP/TCP。Web通过文件、控制器和API管理业务，不是C++工作线程的一部分。

### 6.2 当前C++线程

`pipeline.cpp`启动ai-capture、ai、rga-overlay、main-venc、sub-venc、alsa-audio、rtsp等线程；RTSP后端另有自己的poll worker。启用CSV和事件日志还会有独立写线程。SDK自身也创建内部线程，所以/proc显示35线程不意味着项目源码显式创建35个线程。

- ai-capture：取独立RGA2输出，放入容量1的最新帧队列。
- ai：同步RKNN推理，完成后立即归还输入MEDIA_BUFFER，发布不可变检测元数据。
- rga-overlay：检测结果更新时生成局部BGRA位图，并在TTL过期时清框。
- main/sub-venc：从各自编码通道取压缩包，统计PTS，投递有界编码包队列。
- alsa-audio：PCM读取、下混、G.711A编码和音频PTS。
- rtsp：把视频/音频交给后端；后端worker管理连接、RTSP协议、分包和非阻塞发送。

### 6.3 所有权与RAII

| 资源 | 所有者/RAII | 释放时点 |
|---|---|---|
| SDK MEDIA_BUFFER | `MediaBuffer`；`Frame=shared_ptr<MediaBuffer>` | 最后一个Frame引用释放后调用`RK_MPI_MB_ReleaseBuffer` |
| DMA FD | 通常为SDK借出的句柄 | 不能把借用FD当独立所有权随意close |
| AI BGR图像 | AiFrame/Frame暂持，OpenCV Mat为短期视图 | 推理返回后立即reset源Frame；视图不能逃逸 |
| RKNN输出 | `OutputLease` | scope结束调用`rknn_outputs_release` |
| RKNN上下文 | `Model` | 析构调用`rknn_destroy` |
| VI/RGA/VENC/绑定 | 对应不可复制类，Pipeline持unique_ptr | 关闭队列、join线程后按依赖逆序拆除 |
| OSD位图 | shared_ptr<cv::Mat>及有限代际保留 | 等待已排队RGA任务不再引用后再回收 |
| 编码包 | 队列、RTSP MediaEvent的Frame引用 | 所有消费者引用释放后归还SDK |

shared_ptr的原因是多个异步阶段共享SDK缓冲所有权，不是“DMA-BUF天然要求shared_ptr”。RGA通道输出是独立硬件缓冲，零拷贝也不表示输入和输出必须是同一地址。

### 6.4 锁与队列

- `LatestQueue<T>`：mutex+condition_variable，满时淘汰旧项；移出旧对象后在锁外析构，避免SDK释放阻塞临界区。
- `BlockingQueue<T>`：mutex+ready/space条件变量，编码包队列满时生产者等待；不能把项目所有队列都说成“只保留最新帧”。
- `DetectionCache`：mutex保护有限历史，shared_ptr<const Detection>共享只读结果；条件变量通知OSD，不再高频轮询。
- `LatencyWindow`：锁内复制512样本滚动窗口，锁外排序算分位数。
- RTSP：媒体事件队列有mutex；客户端集合和socket主要由单个worker独占。
- Pipeline：state_mutex、error_mutex及原子运行/失败/统计变量，停止时close队列、notify等待者、join后释放硬件。
- `std::move`用于配置、帧、音频vector和队列元素转移；它本身不执行DMA拷贝，也不保证线程安全。

## 7. 格式、零拷贝与性能指标的准确含义

### 7.1 零拷贝范围

已审计主/子原始图像路径为VI NV12 DMA-BUF→RGA→VENC，无应用级NV12整帧CPU映射/复制，运行不变量是`cpu(nv12=0)`。

以下开销仍存在：

- AI将RGA图像映射供CPU访问，执行letterbox/resize形成模型输入；NPU内部import/copy不可由该旧SDK完整观测。
- OSD在CPU生成小块BGRA位图，由RGA叠加；不是CPU绘制完整1080p帧。
- H.264压缩输出映射到CPU，执行NAL解析、RTP封装和TCP发送。
- 音频PCM编码和网络缓冲发生普通CPU内存读写。

因此不能宣传“包括AI、OSD、网络全链路绝对零拷贝”。DMA cache-sync正确、缓冲生命周期有界和原始帧避免整帧CPU复制才是当前明确证据。

### 7.2 DMA指标

- acquired/released/live：应用获取的MEDIA_BUFFER租约；运行中live不必恒为0，应保持有界。
- VENC压缩输出可能没有DMA FD，`missing`可能正常；RGA原始图像缺失必需FD才记`required_error`。
- `overdue`、`release_error`、`sync_error`、`WARNING=buffer_lifecycle`需要调查。
- process_dma和kernel_dma含SDK池内缓冲，它们不等于应用live租约。
- 各硬件池的精确占用SDK不公开，当前明确标记`not_exposed_by_RKMedia`，不能伪造百分比。
- RGA/VENC获取等待时间含帧周期；不是硬件单次执行时间。AI低频200 ms取帧等待可属于正常5 FPS节拍。

### 7.3 时延和CPU口径

- `server_total`等只到板端发送接口，不包含完整无线、客户端缓存、解码和显示。
- 滚动分位窗口通常512样本，`total`是累计观察次数，不能把窗口分位当全天全量分位。
- `--latency-csv`提供逐样本CSV；绘图脚本可重算全量P50/P95/P99。
- 媒体进程CPU、整机CPU busy、单核100%口径必须区分。四核总容量口径13.1%约等于单核100%口径52.4%。
- 温度较低不等于已经证明无降频；仍需CPU/NPU频率和thermal状态曲线。

## 8. 模型、训练与新增类别

### 8.1 当前模型契约

- YOLOv5 v6.0固定上游commit `956be8e642b5c10af4a1533e09084ca32ff4f21f`。
- YOLOv5n，输入320×320，训练前将SiLU替换为ReLU，第一代RKNN INT8量化。
- ONNX输入RGB float32 NCHW，/255；RKNN API输入RGB uint8 NHWC，由Toolkit mean/std完成归一化。
- 居中letterbox，填充114；不能额外重复除255，也不能忽略填充逆映射。
- 三路raw heads，grid 40×40、20×20、10×10；23类每头通道3×(23+5)=84。
- 模型输出不含完整sigmoid/解码/NMS，C++负责处理；anchors必须使用训练元数据。
- 第一代tensor dims顺序与Toolkit2示例不同，代码已按实际输出验证。
- `objects.rknn`、`objects.json`、`labels.png`必须作为匹配集合交付。

### 8.2 数据与精度状态

- 初始11类COCO子集：437训练图、105验证图。
- 23类：1362训练图、315验证图，训练/验证取不同COCO分区。
- Git保存selection/manifest和模型等必要产物，不上传图片、标签数据集、虚拟环境和下载缓存。
- 这些是小数据迁移学习基线，不是生产泛化精度保证。
- 旧11类约0.303或0.312 mAP@0.5等历史数字不得作为当前23类模型精度。
- 23类历史板端单图三次总耗时平均21.893 ms，包含预处理/推理/后处理；不是当前纯NPU耗时或持续视频FPS。
- 杯/瓶、显示器/笔记本存在混淆与重复框；应补真实镜头、角度、光照、遮挡和负样本，建立独立测试集。

### 8.3 新增类别正确流程

1. 定义类别与ID，确定旧ID兼容策略。
2. 收集真实图片并完整标注；划分训练、验证、独立测试，不以同一视频邻帧泄漏到各集合。
3. 准备固定训练配置与ReLU模型，训练并记录逐类P/R/AP、混淆和小目标表现。
4. 导出ONNX，比较PyTorch与ONNX raw输出。
5. 用代表性校准图进行RKNN转换，核对输入/输出布局及元数据。
6. 重新生成中文图集；执行板端单图和多场景检测。
7. 使用独立模型包做签名A/B部署，验证健康、精度、功耗、温度与吞吐，保留回滚。

关键脚本为`prepare_data.py`、`remap_pretrained.py`、`train.py`、`export_onnx.py`、`prepare_calibration.py`、`convert_rknn.py`、`generate_labels.py`。训练包装器拒绝`--resume`；用已有weights微调不等于恢复原优化器状态。

## 9. Web、安全、OTA和云端

### 9.1 Web与API

访问`https://<板IP>/`。Python仅监听127.0.0.1:8080；外部8080被拒绝是预期。80仍提供厂商页面，不应描述成所有HTTP端口已关闭。

管理令牌从板端本地取，勿写入Git或交接文档：

```bash
adb shell '/root/rv1126_objects/rv1126-web token'
```

主要接口：

```text
GET  /api/health
GET  /api/status
GET  /api/metrics
GET  /api/config
POST /api/config
GET  /api/events
GET  /api/files
GET  /api/files/download
GET  /api/logs/export
GET  /api/models
POST /api/model/upload
POST /api/model/activate
POST /api/restart
GET  /api/security/status
POST /api/auth/token/rotate
POST /api/auth/token/invalidate
GET  /api/ota/status
GET  /api/ota/history
POST /api/ota/upload
POST /api/ota/verify
POST /api/ota/apply
POST /api/ota/rollback
```

有鉴权不代表每个功能已完成真机验收。NPU负载/频率来自BSP暴露节点，读不到时应显示未知，不得填0冒充实测。

### 9.2 TLS与凭证边界

- 当前使用本地设备CA签发的服务端证书；Windows导入的是公开根证书。
- CA私钥、OTA签名私钥、设备TLS私钥、Web令牌、云端设备凭证分别管理。
- 板端需要自己的TLS私钥，但不能保存离线CA或OTA签名私钥。
- 私有地址信任需要CA被客户端信任且SAN匹配；历史192.168.137.109签发成功不代表当前172.20.10.10已验证匹配。
- `curl -k`只能用于连通性诊断，不能作为浏览器可信TLS验收。
- 公开CA副本在本机桌面`rv1126-device-ca.crt`。私钥在仓库外；需要时由用户通过安全渠道提供路径，不在文档中复制密钥。
- 聊天历史曾出现云产品/设备密钥截图及订阅令牌，不能将其作为可公开交付配置；仍有效的暴露凭证应轮换。

### 9.3 已实现的OTA

- 离线RSA-3072/SHA-256签名；SHA-256用于完整性，签名用于发布来源认证，两者不同。
- 清单包含产品、设备型号、架构、版本、单调序号、最低兼容版本和逐文件摘要。
- schema2还包含发布时间、通道、组件、基础版本和依赖。
- 先校验清单签名，再对声明的普通文件做安全解包和逐项摘要检查；拒绝路径穿越、错误设备、篡改、重放和未授权降级。
- full/application/model/web组件与文件级overlay；不是二进制块级差分。
- 备用目录完成后原子切换软链接，60秒健康门禁不通过切回上一已验证版本。
- 事务日志、开机恢复、公钥吊销、连续失败锁定、按已验证状态保留版本。
- OTA目前针对应用/模型/Web，不更新boot/kernel/rootfs。

有明确真机证据：v1.11.1重放拒绝、令牌限速轮换、故障候选60秒回滚；v1.15.x正常签名升级；正常物理冷启动自动恢复。

仍需区分：正常拔电重启通过，**不等于正在OTA切换时拔电回滚通过**。后者需专门构造事务中断并验证`power_loss_rolled_back`。

独立Web模型上传当前以SHA/格式/健康回退为主，不应直接视作与签名模型OTA完全等价。产品化应审查并统一两条模型更新入口的签名和授权策略。

### 9.4 云端草稿和明确需求

用户要求录像不上云；设备状态、CPU/NPU、温度、内存、帧率、检测事件和统计、版本/Git、故障日志、截图及模型元信息可上传。

设计为开发板主动HTTPS上报，断网写有界本地队列；云FC写SLS并签发OSS截图上传地址，开发板不持有SLS/OSS长期AccessKey。

本地草稿`deploy/cloud/`记录上海区规划：FC `rv1126-cloud-api`、role `rv1126-fc-role`、OSS `rv1126-vision-2`、SLS Project/Logstore `device-runtime`，设备名`rv1126-003`。这些是规划/控制台历史信息，必须到实际云账号核对当前资源，不能视为端到端联调成功。

尚缺正式入口URL、服务部署/鉴权验收、RAM最小权限审查、设备HMAC安全配置、重放持久化、实际上传和费用/限额确认。草稿提到的部分打包脚本可能尚不存在，应先逐项核对。

远程重启、模型切换等云下行暂未开放；MQTT/WebSocket只是扩展方向。首次接入建议先只做上报，再独立设计命令签名、权限与防重放。

## 10. 已解决或已规避的问题：现象→原因→办法

| 问题/现象 | 原因或证据 | 已采用处理与剩余边界 |
|---|---|---|
| 有视频没有检测框 | 使用`--no-ai --no-osd`，或方向错误、未检出目标 | 恢复AI/OSD、rotation270、核对检测计数与阈值；没有框不一定是绘制线程失败 |
| 两路VENC超时 | 本来就有主子编码器；启动无帧或稳态取包等待不足 | 先区分启动与稳态，再查VI/RGA绑定；AI取帧超时按低频采样周期调整 |
| 视频偏蓝/红蓝反转 | 本BSP RKMedia格式枚举和RGA实际字节排列不宜同名直译 | 验证RGB/BGR映射并修正；AI代码校验格式/stride。不要移植后不测就改回同名枚举 |
| 框映射错误、横竖屏错位 | 旋转与主子尺度、letterbox坐标混用 | 明确旋转后尺寸；逆letterbox再按同视场主子坐标比例映射，保留class/score |
| 元数据stride解析错误 | OpenCV3.4直接将JSON浮点节点转int不符合预期 | 读double并lround，校验头输出与anchor，增加解码回归 |
| 旧4K路径约29.x FPS | 4K输入下TNR/RKISPP负载与旧路径吞吐问题，曾伴热节流 | 当前改用IMX415 binning 1080p，使TNR输入降为1080p，保留推荐记录区域视场；外部TCP掉帧另查 |
| 原生4K RTSP失败 | 4K硬件链路约53 ms/帧且旧发送端码流损坏 | 不把原始4K输出作为当前默认；保留失败报告，不宣称仅调码率已解决 |
| 4K RAW10→ISP缩小→独立ISPP+TNR硬卡死 | 旧RKISPP DDR输入、TNR首帧、输出缓冲/启动路由耦合 | r2/r3/r4及内核回移植曾研究，未成为当前稳定方案；用户要求停止该方法，回到已验证应用路径 |
| CPU过高 | 主流整帧RGB/CPU绘制、AI先全帧转换后丢弃、频繁轮询 | 主子NV12直绑；RGA局部位图OSD；AI输入端限速；检测事件通知；仍有AI/网络/录像开销 |
| AI缓冲持有约200 ms | 推理完成后Frame一直留到下一低频样本覆盖 | 推理完成即reset，历史P50约18 ms，峰值租约下降 |
| DMA缺FD告警假阳性 | 旧BSP VENC压缩输出可无DMA FD | 区分encoded missing与raw required_error，保留真实cache/释放告警 |
| OSD周期闪烁风险 | 旧AI3 FPS约333 ms，TTL300 ms短于更新周期 | 默认TTL500 ms，显式空检测仍清框；真实持续目标录像待正式验收 |
| 旧librtsp TCP NAL/CSeq损坏 | 旧同步发送与并发/拥塞路径可靠性不足 | 当前自建异步非阻塞TCP/RTP后端；已规避旧库损坏机制，网络帧数不足仍存在 |
| live555候选不能稳定替换 | 候选测试出现28.11 FPS与CSeq异常 | 没有将其当正式后端；不能笼统声称换库就全部解决 |
| 强制IDR后DTS倒退 | 新IDR可越过已排队旧P帧，时间戳顺序反转 | 按客户端拒绝非递增RTP视频时间戳，保护解码；丢弃本身可能损失帧数，非零丢帧方案 |
| Web初启误判不健康 | ISP/VENC预热或日志计数尚未建立 | 增加等待稳态/重试的管理检查；诊断刚启动时仍需区别暖机与实际停流 |
| 开机缺共享库、看门狗未守护 | Buildroot init环境不等于交互shell；进程分离不完整 | 控制器显式PATH/LD_LIBRARY_PATH，start-stop-daemon守护，独立watchdog；故障注入已验证 |
| A/B回滚后旧程序解析参数失败 | 新事件参数写入新旧版本共享runtime.args | v1.14.1改由新版控制器按规则动态追加，旧版本共享参数保持兼容 |
| 回滚后8080重新对外暴露 | 安全绑定仅放在可能回滚的Web脚本里 | 把回环绑定政策放到稳定OTA运行时包装器 |
| 录像事件清单为空/多保留后续片段 | 早期PCM appsrc背压，MKV最终化mtime变化，等待期混入事件窗口 | 原生PCMA重封装；按文件名计算时间；v1.15.1删额外分片补偿，v1.15.2固定事件结束时间并延迟等落盘 |
| 断电后1970、录像不启动 | RTC低电压/未保时，网络未通导致NTP失败 | SNTP重试和RTC写入；错误时间时录像主动等待，校时后自动恢复 |
| Wi-Fi开机连接又被关闭 | 遗留S99z-ip与ConnMan/新服务冲突 | 禁用并备份遗留脚本；确定性wpa_supplicant控制器 |
| ConnMan服务丢失 | 精简BSP持久化不可靠 | WPA凭证置/userdata受限文件，开机自动扫描；真实冷启通过 |
| 同名热点连上但无网络 | 静态地址仍属于旧网段；曾10.203.30与192.168.137混用 | DHCP探测当前网段再修配置；长期DHCP/迁移管理仍需升级，不能说已普适解决 |
| FFmpeg测试0帧或脚本误用 | 地址/握手、重定向、时间戳计时与墙钟计时混用等不同原因 | 分开ffprobe探测、客户端首帧等待、墙钟计数；空日志不表示零错误且业务正常 |
| Git“不是仓库”、脚本找不到 | 在Windows用户目录直接git pull或把“项目目录”当实际路径 | 先克隆/进入真实仓库；Linux venv不可直接复制给Windows运行 |

以上有些是问题规避或局部修复，不表示整个相关模块所有需求已经验收通过。

## 11. 最新未通过项与拥塞诊断详解

### 11.1 三小时正式数据

2026-11-14 16:32:26～19:32:40，3小时14秒，18轮TCP双流，6通过/12帧数不足；板端健康18/18、音视频可解码3/3。

有效双流计数窗口10485秒，理论每路314550帧。主流309987帧（29.565 FPS），子流313419帧（29.892 FPS）。计数不足不能直接换算成无线丢包数。

用户确认曾人为关闭热点约10秒；板端断开至重新关联约25.8秒，含恢复扫描/认证。第8轮作为外部干扰单列。统计排除此轮后仍有11轮不满足门槛，不能将整轮FAIL改为PASS。

资源：171条CSV、179组快照；媒体PID恒定883、线程35、FD160～163；RSS148.28→145.71 MiB，峰值204.96 MiB后回落。CPU四核容量口径平均13.10%、峰值16.73%，不是整机全部进程占用；温度CPU57.1℃、NPU54.8℃。内核DMA91对象/180633600字节稳定，录像样本均健康。样本可达0失败仍漏掉短时Wi-Fi断连。

### 11.2 已复现的外部传输背压

21:29:49，**录像已暂停**，外部主流`172.20.10.5:51732`：queued=517737、limit=524288字节，kernel_outq=78882；TCP unacked=27、lost=27、cwnd=1、RTO约893 ms；该连接累计重传78次。

随后发生清队列/IDR恢复，客户端出现约2.533秒PTS间隔，180秒主流5327帧、约29.594 FPS，子流约30 FPS。

同时间窗无线txretrans、txfail有增量，Ubuntu接收队列没有长期高堆积迹象。已明确故障位于外部TCP传输路径；尚不能唯一指向板端Wi-Fi硬件、驱动、iPhone、无线环境或VMware NAT。

### 11.3 已复现的本地录像停读

恢复录像后，21:32:25本地连接`127.0.0.1:40362`背压，TCP lost/retrans均0、kernel_outq=124822字节；接收端/proc队列曾积到2723408字节（约2.60 MiB）。

录像管理器记录`recorder made no file progress for 30 seconds`，重启1次后恢复；同期外部主流约30.017 FPS，无明显新PTS大间隔。这证明局部录像接收停顿可以与外部传输问题分别发生。

它发生于恢复录像后的启动阶段，尚不能断言是磁盘慢、splitmuxsink最终化、时间戳等待或具体GStreamer元素。应采集元素进度、队列、线程栈与IO耗时后再修。

### 11.4 当前RTSP恢复机制的取舍

每客户端独立有界队列，发送非阻塞。超限时保留半发送TCP记录、丢弃尚未发送的完整媒体记录、等待IDR恢复。这保护协议 framing和有限延迟，但**主动丢帧与无条件零丢帧目标不兼容**。

`df92094`只增加peer/session/epoch/TCP_INFO/TIOCOUTQ等诊断，没有修改该策略，也没有发布新的修复版本。此前“一个录像客户端同步堵住所有客户端”的描述不符合当前单客户端队列设计；共享强制IDR是否影响其他客户端也需要具体事件证据。本轮未出现非单调时间戳丢弃记录。

## 12. 未完成功能与验收缺口

### P0：先解决现有可靠性

1. 外部链路：固定热点，Windows直接拉流对照Ubuntu NAT/桥接，分别记录TCP丢失、重传、cwnd/RTO、MAC失败与客户端队列；避免仅凭RSSI判断无线质量。
2. 录像：针对停止后重启复现，增加GStreamer逐元素进度/队列、线程栈、写盘与分片耗时，定位停读环节；区分启动等待与稳态故障。
3. 接收统计：保留每轮PTS和阶段时间，覆盖握手/重连开销；明确严格零丢帧与容差门槛的区别。
4. 修复后重新做3小时，人工断网作为独立故障恢复测试；通过后再从0计时24小时。

### P1：完成已有产品功能的正式验证

- 主/子流、AI、OSD、音频、录像同时开启的连续3小时/24小时严格验收。
- 长期RSS、DMA、FD、线程、NPU错误和频率/温度曲线。
- OSD持续目标防闪烁、坐标和多框录像，真实区域/越线事件。
- 音画长期漂移：同一持续播放会话，基线/1/3/6/12/18/24小时拍手或LED+蜂鸣器标记；测相对偏移变化。
- 当前音频PTS按采样时长累加，偏离单调时钟超过约100 ms会重置；需记录重对齐事件。RTSP尚未发送完整RTCP Sender Report，不能用不明基准的音视频RTP数值直接相减。
- 24小时视频90 kHz RTP时间戳必跨32位回绕（周期约13小时15分），须验证播放器处理；起点未知，不一定在开测第13小时回绕。
- 光学端到端至少100有效样本，报告P50/P95/P99和最大值；VLC低缓存和默认缓存分别测。
- 签名OTA切换中真实拔电、恢复策略、公钥轮换/吊销、组件升级与失败锁定真机矩阵。
- 镜头对应1080p binning IQ/ALSC标定及不同增益、曝光的画质验证。

### P2：已提出但尚未正式上线

- 阿里云FC/SLS/OSS上报和截图预签名，离线有界补传、幂等/防重放、费用/限额；录像禁止上传。
- 统一模型、应用、Web的签名更新策略；模型灰度、精度回归和一键回退。
- Wi-Fi DHCP租约、热点网段变化自动适配及Web配网，不依赖手工固定地址。
- 日志轮转、磁盘配额、证据保留和下载，防止长测/故障重复日志填满存储。
- 按端口/服务最小化暴露，审查厂商80/FTP/ADB TCP等遗留服务。当前HTTPS/Bearer不等于整块开发板已完成安全加固。

## 13. 后续可开发功能及建议顺序

| 优先级 | 方向 | 建议交付物和验收 |
|---|---|---|
| 第一阶段 | 可靠性收敛 | 客户端级拥塞指标、录像停读修复、3h与24h原始数据/报告 |
| 第二阶段 | 可观测性 | 统一结构化指标、故障前后快照、日志轮转、告警、CPU/频率/温度曲线 |
| 第二阶段 | 音视频同步 | 可观测音频校准次数、ALSA时延、RTCP SR、长期偏移曲线和光学样本 |
| 第三阶段 | 模型精度 | 现场数据与独立测试集，逐类AP/误检/召回、困难样本闭环 |
| 第三阶段 | 事件能力 | 停留/徘徊、目标计数、方向统计、事件截图；先明确目标场景和误报门槛 |
| 第四阶段 | 录像体验 | 事件时间轴、检索、受控导出、录像断点/损坏检测、分片异步与存储寿命评估 |
| 第四阶段 | 网络适应 | 动态码率/分辨率或可配置拥塞策略；UDP/WebRTC/SRT等需按BSP资源另行验证 |
| 第五阶段 | 云管理 | FC上报、SLS查询、OSS图片、设备身份；下行命令独立授权与签名 |
| 第五阶段 | 产品化 | OTA故障矩阵、离线恢复、密钥轮换、审计、权限划分、发布CI |

升级到更新YOLO/检测模型是研究项：优先保持当前可运行基线，分别比较算子兼容、量化精度、RKNN推理时延、CPU后处理及热稳定性。不能只比较PC端参数量后直接替换模型。

## 14. 源码导航：按问题找文件

| 需求/问题 | 入口 |
|---|---|
| 命令行、主进程 | `deploy/pipeline/rkmedia_rockx_face_detection_rtsp.cpp`，文件名保留历史但目标输出是物体检测程序 |
| 参数默认/边界 | `deploy/pipeline/include/vision/config.hpp` |
| 当前线程、初始化、停止、AI、OSD、音频PTS | `deploy/pipeline/src/pipeline.cpp` |
| VI/RGA/VENC/RKAiq/ALSA/格式/传感器模式 | `deploy/pipeline/src/rk_backend.cpp` |
| 缓冲RAII与硬件类接口 | `deploy/pipeline/include/vision/rk_backend.hpp` |
| 队列、检测缓存、锁、分位统计 | `deploy/pipeline/include/vision/core.hpp` |
| RTP/RTSP、TCP队列和诊断 | `deploy/pipeline/src/rtsp_server.cpp` |
| RKNN模型和中文位图 | `deploy/objects.cpp`、`deploy/objects.hpp` |
| YOLO解码、NMS | `deploy/decode.hpp` |
| G.711A | `deploy/pipeline/include/vision/audio.hpp` |
| 事件跟踪/规则 | `deploy/pipeline/src/analytics.cpp`、`include/vision/analytics.hpp` |
| DMA审计 | `src/dma_audit.cpp`、`include/vision/dma_audit.hpp`及docs |
| CSV后台写 | `deploy/pipeline/include/vision/latency_trace.hpp` |
| 媒体控制器/看门狗 | `release/rv1126/objects/rv1126-object` |
| Web后端/前端 | `deploy/web/rv1126_web.py`、`web-app.js`、`web-index.html`、`web-styles.css` |
| HTTPS配置 | `deploy/security/nginx-rv1126.conf` |
| OTA管理 | `deploy/ota/rv1126_ota.py`、`rv1126-ota-recover`、相关测试 |
| Wi-Fi、校时、录像 | `deploy/network/`、`deploy/time/`、`deploy/recording/` |
| 签名打包/安装 | `scripts/build_ota_package.py`、`install_ota_runtime.sh`、`install_https.sh` |
| 模型与训练产物 | `config/`、`scripts/`、`runs/`、`artifacts/` |
| 预编译发布集合 | `release/rv1126/objects/`、`web/`、`SHA256SUMS` |
| 原人脸工程 | `15_rkmedia_rockx_face_detection_rtsp/`及`release/rv1126/face/` |
| 云草稿 | 未跟踪的`deploy/cloud/`，不能假定克隆后存在 |

同名文件在deploy和release中可能各有副本。改源码后必须构建、更新正确release文件、重新校验并签名，不能只改deploy脚本就假定OTA使用了新版本。

## 15. 构建、测试与发布操作

### 15.1 主机C++回归

```bash
cd /home/alientek/桌面/rv1126_object_training
cmake -S deploy/pipeline -B /tmp/rv1126-handoff-host \
  -DRK_VISION_BUILD_DEVICE=OFF -DBUILD_TESTING=ON
cmake --build /tmp/rv1126-handoff-host -j2
ctest --test-dir /tmp/rv1126-handoff-host --output-on-failure
```

核心为core、decode、dma_audit、analytics四组；`ctest`指向错误/空目录可能没有实际运行用例。必须确认测试数量而不只看退出码。

### 15.2 ARM构建

```bash
cmake -S deploy/pipeline -B deploy/pipeline/build-rv1126-sdk -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/rv1126.cmake \
  -DRK_VISION_BUILD_DEVICE=ON -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build deploy/pipeline/build-rv1126-sdk -j2
file deploy/pipeline/build-rv1126-sdk/rkmedia_object_detection_rtsp
```

工具链文件在源目录的`cmake/toolchains/rv1126.cmake`。CMake第一次配置使用源目录相对路径；已有缓存时遵循已有构建配置，必要时用绝对路径。支持通过`RVSDK`或CMake缓存指定SDK位置。

期望输出ARM 32位ELF，不能将主机x86程序上传板端。主机程序查找与目标头文件/库查找根隔离，避免将x86 OpenCV误链接到ARM程序。

### 15.3 Python/前端回归

```bash
python3 -m unittest discover -s deploy/time/tests -v
python3 -m unittest discover -s deploy/recording/tests -v
python3 -m unittest discover -s deploy/web/tests -v
python3 -m unittest discover -s deploy/ota/tests -v
node --check deploy/web/web-app.js
node scripts/tests/test_optical_latency_core.js
bash scripts/verify_checkout.sh
```

命令是接手后的验证入口，本次整理文档没有重新宣称全部测试已运行。历史通过数量与当前源码测试数量可能不同，记录实际输出。

### 15.4 正式发布

1. 在独立开发分支实现与回归，保留旧正式版本。
2. 更新版本、变更日志和release文件，核对`VERSION`一致及SHA256SUMS。
3. 明确生成程序、模型、JSON、图集和Web文件所对应的提交。
4. 使用仓库外离线私钥构造签名包，按`handoff/SECURE_OTA.md`执行。
5. 上传、验签、构造备用目录、切换和健康检查，失败自动回滚。
6. 真机确认后提交报告，再固定新发布标签、同步GitHub和Gitee。

不要把诊断提交df92094当成已发布v1.15.4，也不要移动v1.15.3标签。正常流程只升级应用；boot刷写属于不同高风险工作，需要单独明确范围和恢复方案。

## 16. 板端运行、访问与数据目录

### 16.1 只读核对命令

```bash
adb devices
adb shell 'readlink -f /root/rv1126_objects'
adb shell 'cat /root/rv1126_objects/VERSION; cat /root/rv1126_objects/GIT_COMMIT'
adb shell '/root/rv1126_objects/rv1126-wifi status'
adb shell '/root/rv1126_objects/rv1126-object status'
adb shell '/root/rv1126_objects/rv1126-object health'
adb shell '/root/rv1126_objects/rv1126-object watchdog-status'
adb shell '/root/rv1126_objects/rv1126-recording health'
adb shell '/root/rv1126_objects/rv1126-time-sync health'
```

控制命令`rv1126-object restart/stop`会中断媒体；仅在需要且任务授权时执行。旧人脸和当前目标检测不能同时占用同一摄像头和8554。

### 16.2 当前访问地址

```text
rtsp://172.20.10.10:8554/live/main_stream
rtsp://172.20.10.10:8554/live/sub_stream
https://172.20.10.10/
```

以上IP仅为最新快照，证书SAN需另核对。不要在正式Wi-Fi性能报告中使用ADB forward替代真实网络。

### 16.3 文件系统

```text
/root/rv1126_objects -> /root/rv1126_releases/v1.15.3
/root/rv1126_releases/<version>/        应用A/B版本
/root/rv1126_ota/                      稳定OTA运行时与安全Web包装器
/userdata/rv1126-ota/
  active.json transaction.json history.jsonl release-state.json
  trusted_keys/ revocations.json policy.json failure-state.json
  persistent/                         配置、令牌、事件、截图、录像等持久状态
/userdata/rv1126-network/
  wpa_supplicant.conf network.conf     0600，不入Git/OTA
/userdata/rv1126-time/status.json
/userdata/rv1126-recording/status.json
/userdata/rv1126-security/tls/         设备TLS证书和设备私钥
/root/rv1126_objects/recordings/continuous/
/root/rv1126_objects/recordings/events/
/tmp/rv1126-object.log
/tmp/rv1126-object-watchdog.log
/tmp/rv1126-web.log
/tmp/rv1126-recording.log
/tmp/rv1126-recorder.log
/var/run/rv1126-object.pid
```

用户要求板端保持当前正确版本，OTA仍应保留必要的已验证回滚版本。清理前分辨活动软链接、持久化目录、回滚依赖和临时候选，不得对`/root`、`/userdata`等广泛目录递归删除。

### 16.4 开机顺序

```text
S97rv1126-ota-recover
 -> S97yrv1126-wifi
 -> S97zrv1126-time-sync
 -> S98rv1126-web
 -> S99rv1126-object
 -> S99zrv1126-recording
```

实际初始化中的Nginx/厂商服务还会更早启动。热点未就绪时WPA扫描，NTP重试；录像等待可信时间。NTP默认成功后约每6小时复核，失败约15秒重试。

## 17. 验收执行入口和判定限制

### 17.1 短测

```bash
BOARD_IP=172.20.10.10
bash scripts/test_rtsp_tcp.sh "$BOARD_IP" 60 30
bash scripts/test_rtsp_av.sh "$BOARD_IP" 10
```

### 17.2 三小时、二十四小时

```bash
OUT="$HOME/桌面/rv1126-3h-$(date +%Y%m%d-%H%M%S)"
bash scripts/test_8h_acceptance.sh "$BOARD_IP" 10800 600 "$OUT"

# 必须在修复、三小时复核通过后，另起独立24小时数据集：
OUT="$HOME/桌面/rv1126-24h-$(date +%Y%m%d-%H%M%S)"
bash scripts/test_8h_acceptance.sh "$BOARD_IP" 86400 600 "$OUT"
```

上面是前台执行示例，不要连续复制两个命令就自动进入24小时。后台可用nohup/setsid并记录PID；脚本本身不保证Windows、VMware宿主和虚拟机不休眠。

脚本每轮重建TCP双流、每小时音视频解码、每分钟资源采样，结束生成report.md/result.txt/resources.csv等。它允许每轮比理论少2帧，且自动PASS没有覆盖所有CPU/内存/音画漂移要求。

已知测试不足：

- Ping是低频抽样，会漏掉短时断网；需要持续网络事件或驱动日志时间戳。
- health证明进程/硬件计数活跃，不证明每个客户端完整收帧。
- FFmpeg/ffprobe配置、暖机、终止边界和缓冲释放会影响计数；不能用声明的r_frame_rate证明实收帧率。
- 旧脚本逐轮原始PTS和成功stderr可能被临时文件覆盖，后续应按轮保存必要证据。
- 3次短音视频解码不证明3小时音画无漂移；无解码报错也不证明视频帧完整。
- exit143可能是计划TERM结束；exit124通常是timeout。不能仅凭退出码判根因。

### 17.3 其他观测入口

- DMA：`scripts/capture_dmabuf_audit.sh`。
- 分段时延：`scripts/capture_runtime_latency.sh`、`plot_latency_csv.py`。
- 光学：`optical_latency_timer.html`、`capture_optical_latency_windows.ps1`、`optical_latency_analyzer.html`。
- OTA断电：`scripts/test_ota_power_loss.sh`，需专门维护窗口与恢复准备。

## 18. 证据索引与数据可迁移性

### Git中可随仓库交付的主要证据

- `artifacts/imx415-binning-1080-validation-20261026.md`：当前稳定传感器模式和视场对照。
- `artifacts/pre-tnr-1080-feasibility-20261026.md`：未成功的精确4K→ISP缩放→独立ISPP/TNR方案。
- `artifacts/native-4k-output-validation-20261024.md`：原始4K输出失败。
- `artifacts/board-validation-objects23-20261024.md`：23类首次候选板载记录，末尾候选结论为历史。
- `artifacts/board-validation-v193-coldboot-20261028.md`：旧RTSP长测失败与限制。
- `artifacts/board-validation-v194-async-rtp-20261029.md`、v195/v196报告：异步发送、时间戳和队列策略历史。
- `artifacts/board-validation-v1150-20261104.md`、v1152报告：校时、录像和事件窗口。
- `artifacts/board-validation-v1153-coldboot-20261111.md`：真实物理冷启动通过。
- `artifacts/rtsp-congestion-diagnosis-20261114.md`：两类背压来源、TCP与队列证据。
- `deploy/pipeline/docs/runtime-latency.md`、`dma-buffer-audit.md`、`optical-e2e-latency.md`：指标准确边界。

### 仅保留在本机的原始日志

- `/home/alientek/桌面/rv1126-3h-iphone-v1153-20261114.zt1MNL/`：三小时数据、原始report与review.md。
- `/home/alientek/桌面/rv1126-congestion-20261114-apxenj6k/`：四阶段诊断、原始PTS、TCP_INFO、主机TCP、无线增量和恢复状态。
- `/home/alientek/桌面/rv1126-3h-v1153-20261114.W9xQCR/`：较早的另一轮测试，不能与iPhone轮混合统计。
- `/home/alientek/桌面/rv1126-24h-v1153-20261114.2vsbbM/`：被用户改为先3小时而中止的早期24小时任务，不是24小时通过数据。
- `/home/alientek/桌面/rv1126-ota-output/rv1126-v1.15.3.rvota`：本机正式签名包，SHA-256 `5751bc7749f2aca10fcc501d90164f37c7c13dd5b036f95260b92009eab95d45`。

这些路径不能保证别的电脑克隆后存在。向另一个AI交付时至少给本文件与仓库；需要复核统计时再附相应原始日志目录。不要附Wi-Fi配置、Web令牌、云凭证、离线私钥或大批录像。

用户提供过另一份约58分钟“8项PASS”文字摘要，原始目录未在本次核查中唯一对应；不能拿它覆盖已落盘三小时FAIL。其CPU口径、帧数时间窗口和网络事件也应核对原始数据。

## 19. 交给另一AI的第一轮工作清单

1. 先读本档案0～2节，确认正式板端与HEAD不同，23类是当前模型。
2. 只读运行git status/log/tag，保留未跟踪cloud；查看实际板IP、VERSION、GIT_COMMIT、软链接和启动ID。
3. 查当前是否有长测进程，避免覆盖`/tmp/rv1126_tcp_test`或重复拉流改变负载。
4. 若任务是定位/报告，只做诊断；若获准修复，再构建可回退候选，记录变更与验证结果。
5. 优先网络Windows直连/NAT对照和录像重启停读；不要再次盲目调TNR、刷内核或换模型来解释传输问题。
6. 保留原始报告，只新增复核说明；人为断网明确标记，不把它当设备自发故障，也不因此删除其他失败轮。
7. 每次变更单独提交，说明验证范围。公开版本固定标签、包含可重建源码和匹配模型；不上传venv/cache/数据集和凭证。
8. 更新STATUS/交接证据，准确写出未完成项；测试进行中、短测通过与正式验收通过必须区分。

可直接复制给另一个AI的提示词：

> 请先读取handoff/PROJECT_HANDOFF_2026-11-14.md，再检查当前Git和板端事实。项目是原始RV1126+IMX415，正式板端v1.15.3/54b0e0f，代码基线df92094仅增加诊断，当前23类YOLOv5n+ReLU、第一代RKNN。主子流H.264+PCMA，RGA多框OSD，Web HTTPS、签名A/B OTA、开机校时与录像已实现。三小时Wi-Fi验收未通过，24小时未完成；已复现外部TCP丢失恢复和本地录像偶发停读两类背压。请按证据推进，不把旧候选、配置帧率、Healthy、编码PTS正常或-k连通性当完整验收。不覆盖正式运行文件、不刷boot/kernel，不提交deploy/cloud或任何凭证。先报告你核对到的事实，再实施任务授权范围内的修改，并记录可回退验证和Git提交。
