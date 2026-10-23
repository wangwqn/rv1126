# 从 Windows 继续学习：先读这里

本包用于把 RV1126 摄像头、RKMedia、RGA、VENC、RTSP、人脸检测和自定义物品检测项目，交给 Windows 宿主机上的 AI 工具继续讲解、审阅和改进。

建议把压缩包解压到纯英文、无空格的路径，例如：

```text
D:\rv1126-study
```

随后在 AI 工具中打开解压后的整个目录，并把下面这段话作为第一条提示词：

```text
请先完整阅读 rv1126_object_training/handoff/AI_CONTINUATION_CONTEXT.md、
rv1126_object_training/README.md、STATUS.md、TESTING.md，
再检查两个项目的源码。这个项目目标板是原始 RV1126，不是 RV1126B；
RKNN 使用第一代 Toolkit 1.7.5。请保留现有可运行版本，先解释再修改，
每次修改都给出 Ubuntu 交叉编译、ADB 上传、单图验证、RTSP 验证步骤。
```

## 包内主要内容

- `15_rkmedia_rockx_face_detection_rtsp/`：原始双码流人脸检测 RTSP 工程，已修复颜色和旋转问题。
- `rv1126_object_training/`：稳定 11 类及候选 23 类物品检测的数据清单、训练、ONNX/RKNN 转换、板端 C++ 与测试结果。
- `rv1126_object_training/handoff/`：历史工作、关键结论、AI 接续上下文及原始文本附件。

## Windows 能做什么

- 阅读、搜索、审查 C++、Python 和 CMake。
- 用 AI 学习 VI、RGA、VENC、PTS、DMA-BUF、RAII、线程与锁。
- 查看训练曲线、图片、标注、ONNX/RKNN 元数据和板端实测截图。
- 使用 WSL2/Ubuntu 或原 Ubuntu 虚拟机继续 Python 训练和 ONNX 检查。

Windows 原生环境不能直接运行 ARM 可执行文件，也不能用本包代替 Rockchip SDK。RV1126 交叉编译仍应在原 Ubuntu 环境完成，因为工具链和 sysroot 在 `/home/alientek/rv1126_sdk`，未放入本包。

## 没有打包的内容

- `.venv-train/`、`.venv-rknn/`：约 4.7 GB，只适用于创建它们的 Linux；用 lock 文件重建。
- `.cache/`：约 879 MB，可重新下载。
- `downloads/rknn-toolkit-v1.7.5-packages.tar.gz`、COCO 标注压缩包及 Ubuntu deb：均可根据 README 中的来源重下。
- `/home/alientek/rv1126_sdk`：体积巨大并受具体 SDK/板卡配置影响。
- 开发板根文件系统：可运行文件和模型已在包内保留。

保留了官方 YOLOv5 v6.0 源码、项目数据集、所有训练运行记录、权重、ONNX、RKNN、板端二进制、日志和实测图片。官方初始化权重 `yolov5n-v6.0.pt` 也单独保留在 `downloads/`。

## 重要安全说明

历史中出现过 Wi-Fi 名称和密码。SSID 保留用于理解网络拓扑，密码已脱敏，不写入交接文档。请勿把开发板保存的 ConnMan 凭据或任何个人凭据上传到公共 AI、GitHub 或网盘。

