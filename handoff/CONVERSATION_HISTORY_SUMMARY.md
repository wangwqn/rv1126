# 相关对话历史摘要

说明：当前工具无法导出聊天产品保存的逐字原始会话。本文件依据本次工作中可见的完整请求序列和实际执行记录整理，供另一个 AI 接续；不是声称逐字无遗漏的聊天导出。原始粘贴文本附件另存于 `attachments/`。

## 问题出现与原工程调试

1. 用户将 `rkmedia_rockx_face_detection_rtsp` 通过 ADB 上传到开发板，并以 `--no-ai --no-osd` 启动，看到两个 VENC channel 的超时日志，询问为何有两路视频。
2. 解释并排查主流/子流：VENC0 对应主流，VENC1 对应子流；RTSP 地址分别为 `/live/main_stream` 和 `/live/sub_stream`。
3. Ubuntu 最初没有 `ffplay`；安装/使用播放器后，曾出现连接 `127.0.0.1:8554` 被拒绝。后续确认需要服务运行、正确 IP 或 `adb forward tcp:8554 tcp:8554`。
4. 用户看到有视频但没有人脸检测；此前命令使用了 `--no-ai --no-osd`，这会明确关闭检测和叠加。
5. 讨论从 USB ADB 转成 Wi-Fi RTSP。开发板先连接 `Xiaomi_ABDF`，后来因客户端隔离问题改用 Windows 热点 `wang`。
6. Windows 宿主机曾对 `192.168.31.193:8554` 测试失败；分析同网段、AP 隔离、虚拟机桥接、路由和端口监听。
7. 最后开发板成功连接 `wang`，地址 `192.168.137.109`；网关 `192.168.137.1`，ping 成功。历史密码不在本交接文档中保存。

## C++ 学习主题

用户连续询问并学习：

- 项目中智能指针的位置和用途。
- 锁机制、mutex、condition_variable、队列和检测结果缓存。
- 视频帧使用 `shared_ptr` 是否因为 DMA-BUF；结论是关键原因是共享 SDK buffer 所有权和确定释放，不是 fd 本身要求 shared_ptr。
- 移动语义、右值引用和 `std::move` 在队列与 Frame 传递中的用法。
- C++ 并发编程、后续升级方向、主流/子流/AI 流之间的线程关系。
- CMake、交叉编译和 Rockchip 多媒体平台。
- VI 取帧与缓冲释放、RGA 格式/stride、VENC 码率/GOP/PTS、RockX、RTSP，以及如何和 RAII/并发结合。
- NV12/RGB/BGR/RGBA、YUV 转换、width/height/stride/virtual stride、H.264、时间戳、旋转坐标、Alpha OSD、DMA-BUF 缓存一致性。

## 颜色、旋转与人脸框

1. 用户报告 RTSP 画面偏蓝，重点检查 RGB/BGR、NV12/NV21、颜色矩阵。
2. 直接修改 `src/rk_backend.cpp`，根据该 SDK 的 RKMedia→RGA 实际映射修复红蓝颠倒，并重新编译部署验证。
3. ADB 一度无法识别开发板；恢复后继续上传。
4. 用户报告人脸检测没有画框。最终用 `--rotation 270` 纠正摄像头方向，并抓帧验证绿色人脸框。
5. 讨论子流意义：降低 AI 和网络开销，主流保留质量；检测结果缩放回主流绘制。

## 新模型需求与实现

1. 用户要求新建目录，训练可部署到 RV1126 的新模型；当时没有自己的训练图和标注。
2. 类别逐步确定为常见物品，并增加电脑、书籍、显示器、常见水果。
3. 最终 11 类为手机、杯子、瓶子、书籍、鼠标、键盘、笔记本电脑、显示器/电视、香蕉、苹果、橙子。
4. 用户要求检测框旁标注名称，因此加入类别 ID、中文标签图集和置信度绘制。
5. 新建 `rv1126_object_training`，使用隔离训练环境和隔离 RKNN 转换环境。
6. 从 COCO 2017 筛选真实图片，得到 437 张训练图和 105 张验证图。
7. 固定 YOLOv5 v6.0，采用 YOLOv5n、320 输入、ReLU；训练、导出 raw heads ONNX、准备量化样本、使用第一代 RKNN Toolkit 1.7.5 转换。
8. 安装旧工具依赖时处理了 Python 头文件、TensorFlow 2.2、PyTorch 1.10 等兼容问题，未修改系统 Python。
9. 编写 RKNN C++ RAII、三头解码、letterbox 坐标逆映射、按类别 NMS、中文 OSD、单图工具和实时双流程序。
10. 首次板端解析元数据时，OpenCV 3.4 将 JSON 浮点 stride 直接转 int 出现问题；改为读取 double 后 `lround`，随后三路输出 40×40、20×20、10×10 全部验证通过。
11. 开发板单图检测耗时约 16.42 ms，检出了鼠标、笔记本电脑和键盘；板端生成中文标注图。
12. 启动实时程序后，AI frame 持续增长，Wi-Fi 主流 H.264 720×1280@20 fps 成功，实时帧显示中文“杯子/瓶子”标签，同时暴露了类别混淆问题。

## 当前交接状态

- 新物品检测程序最后以后台方式运行过，PID 文件 `/tmp/rkvision-objects.pid`，日志 `/tmp/rkvision-objects.log`。重启后不会自动启动。
- 重启后的手动启动命令见 `README.md` 和 `TESTING.md`。
- 项目不包含明文 Wi-Fi 密码。
- 训练和板端实测状态见 `STATUS.md`。
- 用户此次要求把所有相关项目文件和历史上下文打包，以便在 Windows 上借助 AI 工具继续学习。

