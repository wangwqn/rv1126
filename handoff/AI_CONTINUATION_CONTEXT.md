# AI 接续上下文

## 用户目标

用户正在学习基于 ALIENTEK RV1126 开发板的 C++ 多媒体与 AI 部署，希望理解并继续完善：

1. 摄像头 VI 取帧、RGA 转换/旋转、VENC H.264 编码、RTSP 双码流输出。
2. DMA-BUF 生命周期、缓存一致性、stride、PTS、颜色格式与坐标映射。
3. C++ 并发、锁、智能指针、移动语义、RAII 和异常安全。
4. RockX 人脸检测，以及自训练目标检测模型经 ONNX/RKNN 部署到原始 RV1126。
5. 检测框旁显示中文类别名称和置信度。

## 硬件与环境事实

- 板卡：ATK-DLRV1126，芯片是原始 RV1126，不是 RV1126B。
- Ubuntu 20.04，Python 3.8.10，无 NVIDIA GPU；训练使用 CPU。
- SDK 原路径：`/home/alientek/rv1126_sdk`，未打包。
- ARM 工具链：GCC 8.3，`arm-linux-gnueabihf`，32 位 hard-float。
- sysroot：`buildroot/output/alientek_rv1126/host/arm-buildroot-linux-gnueabihf/sysroot`。
- 板端 RKNN runtime：1.7.0，driver `6.4.6.5.351518`。
- 转换工具：第一代 RKNN Toolkit 1.7.5；Toolkit2 不适用于这块原始 RV1126。
- 摄像头安装方向对应 `--rotation 270`。
- Windows 热点 SSID 为 `wang`，最后一次开发板 DHCP 地址为 `192.168.137.109`；地址重启后可能改变。密码已脱敏。

## 原人脸检测工程

路径：`15_rkmedia_rockx_face_detection_rtsp/`。

它有主流、子流和 AI 路径：

- 主流：1280×720 输入，旋转后 720×1280，3 Mbps，H.264，RTSP `/live/main_stream`，绘制 OSD。
- 子流：640×360，旋转后 360×640，600 kbps，RTSP `/live/sub_stream`，也是 AI 取帧源。
- AI：默认 5 fps，从子流共享 `MEDIA_BUFFER`，异步发布检测结果；主流按 PTS 选择新鲜结果并缩放坐标。

关键修复：这个 SDK 的 RKMedia RGB/BGR 枚举与 librga 格式名称反向对应。`src/rk_backend.cpp` 中映射为：

```cpp
IMAGE_TYPE_RGB888 -> RK_FORMAT_BGR_888
IMAGE_TYPE_BGR888 -> RK_FORMAT_RGB_888
```

这样修复了 RTSP 画面偏蓝/红蓝颠倒。不要未经验证改回同名映射。

缓冲所有权：`Frame = std::shared_ptr<MediaBuffer>`。`MediaBuffer` 析构时调用 `RK_MPI_MB_ReleaseBuffer`，DMA fd 只是借用句柄，不表示所有权。队列只保存最新帧，满时淘汰旧帧；析构在锁外发生。`DetectionCache` 用 mutex 保护不可变检测结果历史。

## 新物品检测工程

路径：`rv1126_object_training/`。

稳定发布的类别顺序固定为：

| ID | 英文 | 中文 | COCO zero-based ID |
| --- | --- | --- | --- |
| 0 | cell_phone | 手机 | 67 |
| 1 | cup | 杯子 | 41 |
| 2 | bottle | 瓶子 | 39 |
| 3 | book | 书籍 | 73 |
| 4 | mouse | 鼠标 | 64 |
| 5 | keyboard | 键盘 | 66 |
| 6 | laptop | 笔记本电脑 | 63 |
| 7 | monitor_tv | 显示器/电视 | 62 |
| 8 | banana | 香蕉 | 46 |
| 9 | apple | 苹果 | 47 |
| 10 | orange | 橙子 | 49 |

“电脑”目前只对应 COCO `laptop`，不包含台式机机箱。不要只改 `classes.json` 就声称增加了类别；类别改变需要重新标注、训练、导出、量化并更新中文标签图集。

v1.8.1 另有一个尚未替换稳定发布的 23 类候选：保持上述 0–10 不变，追加
person、backpack、handbag、bowl、sandwich、chair、couch、potted_plant、
dining_table、remote、clock、scissors。配置在 `config/classes-v2.json`，候选包在
`release/rv1126/candidates/objects23/`。它已完成主机训练、ONNX 数值检查和
RKNN Toolkit 1.7.5 转换；由于开发板当时离线，不能宣称已经通过 NPU 或实时流验证。

### 数据

- COCO 2017 真实图片和官方 YOLO 标注。
- 437 张训练图、105 张验证图；两个分区的图片 ID 不重叠。
- 数据清单及每类实例数在 `datasets/coco_objects/manifest.json`。
- 图片可能包含多个所选类别，脚本保留图片内所有 11 类标注。
- 23 类候选数据为 1362 张训练图、315 张验证图，选择与统计清单位于
  `datasets/coco_objects_v2/`；图片和标签同样不进入 Git。

### 模型与训练

- 官方 YOLOv5 v6.0，固定 commit `956be8e642b5c10af4a1533e09084ca32ff4f21f`。
- YOLOv5n，输入 320×320；为了第一代 RV1126 NPU兼容，在模型创建前将 SiLU 替换为 ReLU，然后训练。
- 初始化脚本从官方 COCO 权重抽取 11 个类别的检测头通道，同时迁移骨干权重。
- 第一轮 12 epoch：总体 mAP@0.5 约 0.303。
- 后续 36 epoch 微调：最佳总体 P=0.439、R=0.312、mAP@0.5=0.312、mAP@0.5:0.95=0.175。
- 小数据基线并非成熟模型；书籍、杯/瓶和部分水果仍弱。

板端最后经过完整验证并运行的是第一轮基线：

- `artifacts/objects-rv1126.rknn`
- SHA256 `661285a3c12e43fd58c2e2018adb1d8da5766bbf43601992b3468854b78402b8`
- RKNN 单张公开验证图：预处理 + 推理 + 后处理约 16.42 ms。
- 检出笔记本电脑、两个鼠标、键盘；中文标签显示正常。
- 实时画面中检测到一个容器，但曾同时判成“杯子”和“瓶子”，说明需补数据。

微调权重 `runs/objects11_refine/weights/best.pt` 已保留，但尚未完成重新导出、量化和板端回归，因此不能称为板端当前版本。

### 模型 I/O 约定

- ONNX：RGB float32 NCHW，除以 255，静态 batch=1。
- RKNN 输入：RGB uint8 NHWC；Toolkit 配置 mean=0、std=255，reorder `0 1 2`。
- 预处理：保持宽高比的居中 letterbox，填充 114。
- 三个输出：stride 8/16/32 的 raw logits；没有 sigmoid、YOLO 解码或 NMS。
- C++ 解码：sigmoid，YOLOv5 xy/wh 公式，letterbox 逆映射，按类别 NMS。
- anchors 从训练权重元数据读取，不能拿另一模型的固定 anchors 替换。
- 第一代 RKNN API 返回的 tensor dims 顺序与 Toolkit2 示例不同，代码已用实际输出验证。

### 中文 OSD

`scripts/generate_labels.py` 用 Noto Sans CJK 预渲染 `labels.png`，板端无需字体库。C++ 从图集复制相应中文名称，再用 OpenCV 绘制百分比。类别 ID 和 score 存在 `Box` 中，并由 `scale_boxes` 原样保留。只写主流帧，AI 子流保持只读。

### 并发与缓冲约束

- 主/子 VI→RGA 采集线程分别进入 capacity=1 的 latest queue。
- 主处理线程选择检测结果、写 OSD、RGA 转 NV12、送 VENC0。
- 子处理线程按预算抽取 AI 帧，同时转 NV12、送 VENC1。
- AI 线程同步调用 RKNN；输入只在调用期间借用；输出由 RAII `OutputLease` 释放。
- RTSP 线程独占 RTSP server，并轮询 VENC0/1。
- DMA CPU 读写必须在 `RK_MPI_MB_BeginCPUAccess/EndCPUAccess` 之间。
- 不要在 AI 和编码共享的子流源帧上写 OSD。

## 已验证测试

- ONNX checker 通过，PyTorch/ONNX Runtime raw-head 数值比较通过。
- host portable core tests 通过。
- NCHW/NHWC 解码、letterbox、按类别 NMS、NaN、class ID 缩放测试通过。
- ARM 交叉编译成功：`object_image` 和 `rkmedia_object_detection_rtsp`。
- 板端 RKNN runtime 1.7.0 能加载 Toolkit 1.7.5 生成的量化模型。
- RTSP 主流：H.264，720×1280，20 fps；ADB 转发和 Wi-Fi 地址都曾成功抓流。
- 实测文件见 `artifacts/device-initial-result.jpg` 和 `artifacts/live-initial.png`。

## 开发板运行方式

上传位置：`/root/rv1126_objects/`。它不会覆盖原人脸程序。原人脸程序和新物品程序不能同时占用摄像头及 8554。

```bash
cd /root/rv1126_objects
./rkmedia_object_detection_rtsp --rotation 270 --threshold 0.35 --ai-fps 5
```

主流：`rtsp://开发板IP:8554/live/main_stream`。子流：`/live/sub_stream`。

## 后续 AI 应优先做什么

1. 不破坏已验证基线；新模型使用新文件名或新目录。
2. 从真实摄像头采集训练/验证/测试数据，尤其补书籍、水果、杯子/瓶子的困难样本和空背景负样本。
3. 检查完整标注，避免未标注目标被当成背景；独立测试集不能参与调参。
4. 重新训练后比较逐类 AP、混淆矩阵、召回率和现场误检，不只看总体 mAP。
5. 重新做 PyTorch→ONNX→RKNN→板端单图→实时 RTSP 全链路回归。
6. 如优化性能，先量测预处理、NPU、后处理、OSD、VENC 分段耗时；不要仅凭单图 16.42 ms 推断持续 FPS。
7. 如配置开机自启，先做可靠的前台/后台停止机制、日志轮转、摄像头/端口冲突处理，再改 init 脚本。

## 不能凭空假设的事项

- DHCP 地址固定不变。
- Windows 宿主机一定能访问热点客户端；需要 `Test-NetConnection` 实测。
- 微调权重一定优于当前 RKNN；它尚未完成板端量化回归。
- COCO 小样本能覆盖用户现场域。
- Linux venv 可以复制到 Windows 使用。
- 原始 RV1126 能使用 RKNN Toolkit2。

