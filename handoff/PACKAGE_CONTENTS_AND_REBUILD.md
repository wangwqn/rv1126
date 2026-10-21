# 打包范围与重建说明

## 已包含

- 两个项目的全部自有源码、头文件、CMake、测试和文档。
- `third_party/yolov5` 固定版本源码及其 Git 元数据。
- `datasets/coco_objects` 的训练/验证图片、标签、缓存和 manifest。
- `runs` 中两轮训练的权重、指标、图表和日志。
- `artifacts` 中初始化权重、ONNX、RKNN、元数据、量化列表/图片、中文标签和测试截图。
- ARM 交叉编译输出和 host 测试输出。
- Python 脚本、requirements、两个环境的完整 `pip freeze`。
- 官方 YOLOv5n v6.0 初始化权重。
- 当前能访问到的两份历史粘贴文本附件。

## 有意排除

```text
rv1126_object_training/.venv-train/
rv1126_object_training/.venv-rknn/
rv1126_object_training/.cache/
rv1126_object_training/downloads/packages/
rv1126_object_training/downloads/python-dev/
rv1126_object_training/downloads/*.tar.gz
rv1126_object_training/downloads/*.zip
rv1126_object_training/downloads/*.deb
```

原因：这些文件合计约 5.6 GB，多数是 Linux ELF 包、重复 wheel 缓存或可下载压缩包，在 Windows 原生 Python 中不可移植。保留它们会超过当前磁盘空间，也不会让 Windows 直接执行交叉编译。

## Ubuntu/WSL 重建训练环境

项目最初使用 Python 3.8。新版本 Python 可能无法安装这些旧版本，建议用 Ubuntu 20.04 或容器：

```bash
python3.8 -m venv .venv-train
.venv-train/bin/python -m pip install --upgrade pip
.venv-train/bin/python -m pip install \
  torch==1.10.2+cpu torchvision==0.11.3+cpu \
  --extra-index-url https://download.pytorch.org/whl/cpu
.venv-train/bin/python -m pip install -r requirements-train.txt
```

精确环境快照见 `requirements-train.lock.txt`。它包含传递依赖，若平台不兼容，应以 `requirements-train.txt` 为首选，并参考 lock 排错。

## 重建 RKNN 转换环境

第一代 RKNN Toolkit 只在 x86_64 Linux 中使用：

```bash
python3.8 -m venv .venv-rknn
.venv-rknn/bin/python -m pip install rknn_toolkit-1.7.5-cp38-cp38-linux_x86_64.whl \
  -c constraints-rknn.txt
.venv-rknn/bin/python -m pip install tensorflow==2.2.0
.venv-rknn/bin/python -m pip install \
  torch==1.10.0+cpu torchvision==0.11.1+cpu \
  --extra-index-url https://download.pytorch.org/whl/cpu
```

官方 wheel 的下载与 MD5 信息已记录在 README 和历史上下文。精确快照见 `requirements-rknn.lock.txt`。RKNN Toolkit2 不能替代这个转换环境。

## 交叉编译环境

需要用户自己的完整 RV1126 SDK，并保持与目标板 rootfs 兼容。默认路径写在：

```text
rv1126_object_training/deploy/pipeline/cmake/toolchains/rv1126.cmake
```

若 SDK 在其他路径，配置 CMake 时传 `-DRK_SDK_ROOT=...` 或设置工程支持的 `RVSDK` 环境变量。Windows 推荐通过原 Ubuntu 虚拟机完成，不建议尝试把 Linux ARM sysroot 直接改造成 MSVC 工程。

