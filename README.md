# RV1126 常见物品检测训练与部署

独立于原 `15_rkmedia_rockx_face_detection_rtsp` 项目，不覆盖原人脸程序。
目标芯片是 **RV1126（不是 RV1126B）**，使用第一代 RKNN Toolkit。

## 克隆后快速使用

仓库包含原始人脸工程、物体检测工程、固定版本 YOLOv5 源码、COCO 数据选择清单、
训练结果、ONNX/RKNN 产物，以及针对当前 ALIENTEK RV1126 BSP 编译的发布包。
不包含不可移植的 Python 虚拟环境、下载缓存、完整 Rockchip SDK 和 CMake 中间文件。
437 张训练图和 105 张验证图不上传，可用已记录的选择参数重新下载生成。

```bash
git clone git@github.com:wangwqn/rv1126.git
cd rv1126
bash scripts/verify_checkout.sh
```

相同开发板/rootfs 可直接上传已经编译好的物体检测发布包：

```bash
bash scripts/deploy_release.sh
adb shell 'cd /root/rv1126_objects && ./rkmedia_object_detection_rtsp --rotation 270 --threshold 0.35'
```

预编译程序依赖当前板载 RKMedia、RGA、RKNN、RKAiq 和 C/C++ 运行库 ABI；不同 BSP
必须按本文后面的交叉编译步骤重新生成。发布包详情见 `release/rv1126/README.md`。

## 检测类别与名称显示

11 类：手机、杯子、瓶子、书籍、鼠标、键盘、笔记本电脑、显示器/电视、香蕉、苹果、橙子。
“电脑”目前指笔记本电脑；台式机机箱、其他水果不属于这个模型的类别。
类别顺序由 `config/classes.json` 固定。改类别后需要重新准备标注、训练和生成标签，不能只改标签文件。

主码流叠加绿色框、中文类别名和置信度，例如“苹果 92%”。
子码流保留无标注的低分辨率预览，AI 从子流取帧，检测结果按坐标缩放到主流。
标签已写入编码前的像素，VLC 无需额外插件。

实时管线默认已升级为 1920x1080@30FPS 主流：VI 使用 V4L2 DMABUF，主、子视频
分别经 RGA NV12 硬件通道直绑 VENC；第三个低分辨率 RGA BGR 通道只供 RKNN。
主流不再转成整帧 RGB，也不再用 OpenCV 修改 1080P 像素。中文名和置信度只在
AI 结果更新时生成小型透明 ARGB 标签条，再由 RGA 与三条硬件边组成标注框。
RV1126 旧 RGA API 只有 8 个区域，因此实时显示最多 2 个带名称框，更多结果计入
`osd.truncated`；检测本身仍保留完整结果。

## 当前数据与模型定位

数据来自公开 COCO 2017 的真实图片及官方类别对应标注，并非生成图。
初始选取每类最多 40 张训练图、10 张验证图，合并去重后是 437 / 105 张。
一张图里的所选类别都保留标注。训练与验证来自不同 COCO 分区，没有图片 ID 交集。
具体文件和各类实例数量见 `datasets/coco_objects/manifest.json`。

这是小数据量的迁移学习/部署基线，不是已经达到生产准确率的成品。
首轮 12 epoch 的验证 mAP@0.5 为约 0.303，书籍和部分水果较弱。
后续微调与实机结果请看 `STATUS.md`；不能将验证指标当成摄像头现场准确率。
需要补充实际摄像头角度、光照和背景的图片及完整标注，并保留独立测试集。

## 目录

```text
config/classes.json        类别顺序、中文名、原 COCO 类别索引
scripts/                   下载标注、训练、导出、量化、推送与预览
third_party/yolov5/         官方 YOLOv5 v6.0，固定 commit
datasets/coco_objects/     本机数据集；仓库只保存选择清单，图片和标签需重新生成
runs/                      训练权重、损失和验证指标
artifacts/                 ONNX、RKNN、模型元数据、中文标签、测试输出
deploy/objects.cpp         RKNN RAII、预处理、后处理、中文绘制
deploy/decode.hpp          三路 raw head 解码和按类别 NMS
deploy/pipeline/           从原工程复制的独立双流视频管线
deploy/pipeline/build-rv1126-sdk/  ARM 交叉编译产物
.venv-train/               PyTorch 训练/ONNX 环境
.venv-rknn/                RKNN Toolkit 1.7.5 / TensorFlow 2.2 转换环境
```

## 训练和导出

在 Ubuntu 中执行；不要在开发板上训练。已有依赖安装在两个隔离环境中。

```bash
cd /home/alientek/桌面/rv1126_object_training

# 已有数据时，参数需与 selection.json 一致。更换采样规模请指定新的 --output。
.venv-train/bin/python scripts/prepare_data.py --train-per-class 40 --val-per-class 10 --resume

# 仅从可信的官方 COCO 权重初始化 11 类输出通道，不是训练完成的权重。
.venv-train/bin/python scripts/remap_pretrained.py

# 此包装器在训练前将 SiLU 改成 ReLU，不会在导出时偷换激活函数。
# --workers 0 避免受限环境中的多进程 socket 问题；普通机器可自行增加。
.venv-train/bin/python scripts/train.py \
  --data datasets/coco_objects/data.yaml --weights artifacts/init11.pt \
  --img 320 --batch-size 8 --epochs 100 --device cpu --workers 0 \
  --project runs --name objects11 --cache ram

# 把下面的权重路径替换为你刚训练出的实际 best.pt 路径。
.venv-train/bin/python scripts/export_onnx.py --weights runs/objects11/weights/best.pt
.venv-train/bin/python scripts/prepare_calibration.py
.venv-rknn/bin/python scripts/convert_rknn.py
.venv-train/bin/python scripts/generate_labels.py
```

再次训练同名目录时，上游会自动增加序号。微调已有本工程权重可用 `--weights .../best.pt`；
这会重新创建优化器，不等同于断点恢复。包装器明确拒绝 `--resume`。
导出会检查类别顺序、ReLU、ONNX 合法性，并对比 PyTorch/ONNX Runtime 原始输出。

RKNN 输入是 RGB uint8 NHWC，由 Toolkit 的 mean=0、std=255 完成归一化；
ONNX 输入是 RGB float32 NCHW /255。两端使用居中 letterbox，填充值 114。
RKNN 三个输出是 raw logits，**没有 sigmoid、解码或 NMS**，C++ 中统一处理。
元数据中的 anchors 来源于训练结果，不能用另一份模型的固定 anchors 替换。

## 交叉编译及上传

```bash
cmake -S deploy/pipeline -B deploy/pipeline/build-rv1126-sdk -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=deploy/pipeline/cmake/toolchains/rv1126.cmake \
  -DRK_VISION_BUILD_DEVICE=ON -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build deploy/pipeline/build-rv1126-sdk -j2
bash scripts/push_device.sh
```

工具链默认 SDK 在 `/home/alientek/rv1126_sdk`。上传目标为 `/root/rv1126_objects/`。
上传脚本不停止现有服务、不改系统运行库、不设置开机启动。
如已有同名新程序正在运行，先在其启动终端按 Ctrl+C，再上传。

先验证一张图片（需上传自己的 `test.jpg`）：

```bash
adb shell
cd /root/rv1126_objects
./object_image objects.rknn objects.json labels.png test.jpg result.jpg 0.35
exit
adb pull /root/rv1126_objects/result.jpg ./artifacts/device-result.jpg
```

确认输出后启动视频。原人脸程序和新程序不能同时占用摄像头或 8554 端口。

```bash
adb shell
cd /root/rv1126_objects
./rkmedia_object_detection_rtsp \
  --width 1920 --height 1080 --sub-width 640 --sub-height 360 \
  --fps 30 --ai-fps 3 --bitrate 6000000 --sub-bitrate 600000 \
  --pool-buffers 8 --packet-queue 8 --rotation 270 --threshold 0.35
```

`--rotation 270` 沿用原摄像头的安装方向；摄像头安装变化时再调整。
退出用 Ctrl+C。不要带 `--no-ai` 或 `--no-osd`，否则无法看到检测标注。
板载 2020 版 `librtsp` 在 8 Mbps 的 TCP interleaved 模式下实测会拥塞并损坏
主流。5 Mbps 双路解码 60 秒通过；6 Mbps 双路解码 60 秒和 120 秒耐久复测均通过，
两路分别完整收到 1800/3600 帧且无协议、H.264 或 PTS 缺口，因此当前默认值为
6 Mbps。这里的“稳定上限”只适用于当前开发板、BSP 和热点网络组合；链路较差时可用
`--bitrate 5000000` 或 `--bitrate 4000000` 主动降码率。

Ubuntu 经 ADB 测试：

```bash
adb forward tcp:8554 tcp:8554
ffplay -rtsp_transport tcp rtsp://127.0.0.1:8554/live/main_stream
```

Windows VLC 经 Wi-Fi：先从 `adb shell ip -4 addr show wlan0` 确认开发板当前地址，
再打开 `rtsp://开发板当前IP:8554/live/main_stream`。
无 wlan0 地址时先恢复网络，不要沿用旧的 DHCP 地址。`127.0.0.1` 只属于做了 ADB 转发的电脑。

## 缓冲区与并发约束

- `Frame = shared_ptr<MediaBuffer>` 持有 RKMedia 句柄，最后一个引用释放时归还缓冲区；DMA fd 不代表所有权。
- 主流与子流都是 `VI(NV12 DMABUF) -> RGA(NV12) -> VENC` 硬件绑定，应用不逐帧映射视频像素。
- AI 独占 RGA2 的 BGR DMA-BUF；输入只在同步 RKNN 调用期间映射读取。RKNN 输出用 RAII 在解码后释放。
- OSD 线程只更新 RGA 区域元数据和小型 ARGB 标签；标签保留四代存储，避免异步 RGA 引用失效。
- 类别编号和得分随检测结果、时间戳一起缓存，`scale_boxes` 保留类别编号；过期结果不再画框。
- 两个 VENC 使用独立取码线程和容量 8 的无丢弃阻塞队列；只有 AI 队列会淘汰过期帧。

程序每 5 秒输出 `main/sub pts_gap`、启动时钟重置 `pts_reset`、AI 采样与淘汰、
OSD 截断和 RTSP 队列高水位。
开发板离线时，ARM Release 编译和主机测试通过不等于 CPU<30%、零丢帧和
端到端<200ms 已达标；必须按 `deploy/pipeline/docs/board-validation.md` 在 IMX415
真机上完成 30 分钟/24 小时、CPU 和画面时钟对拍测试。

## 依赖与来源

- [YOLOv5 v6.0](https://github.com/ultralytics/yolov5/tree/v6.0)，commit `956be8e642b5c10af4a1533e09084ca32ff4f21f`；上游代码及权重保留其原有许可。
- [COCO](https://cocodataset.org/)；标注来源记录在下载脚本，图片使用条件见原数据集及图片来源。
- [RKNN Toolkit](https://github.com/rockchip-linux/rknn-toolkit)，使用第一代 1.7.5，不使用 Toolkit2。

训练依赖见 `requirements-train.txt`，CPU PyTorch 为 `1.10.2+cpu`、torchvision 为 `0.11.3+cpu`。
转换环境使用官方 cp38 x86_64 wheel，补装 TensorFlow 2.2.0，传递依赖约束见 `constraints-rknn.txt`。
缺少 Python 头文件时，本项目只解压了 Ubuntu `libpython3.8-dev` 包供旧依赖编译，没有升级系统 Python。
所有 `.pt` 都通过 pickle 加载，只使用可信来源或自己训练的权重。
