# RV1126 IMX415 双码流人脸检测与 RTSP

本项目面向 RV1126、IMX415、RKMedia、RGA、VENC 和 RockX。默认目标配置为
`1920x1080@30FPS` 主流、`640x360@30FPS` 子流、H.264 CBR，AI 按 5 FPS
从子流采样。程序使用 C++14 和 RAII 管理 RKMedia 句柄、DMA-BUF 租约、线程、
绑定关系及模型实例。

## 优化后的数据流

```text
IMX415 -> ISP/ISPP
  |
  +-> VI0 1920x1080 NV12 (V4L2_MEMORY_DMABUF)
  |    -> RGA0 NV12（旋转 + 硬件区域画框）
  |    -> RKMedia bind -> VENC0 H.264
  |    -> VENC0独立取码线程 --+
  |                            |
  +-> VI1 640x360 NV12 --+-> RGA1 NV12 -> VENC1 -> VENC1独立取码线程 --+
                         |                                        |
                         +-> RGA2 BGR888 -> AI采样线程             |
                              -> AI最新帧队列[1] -> RockX          |
                              -> 检测元数据 -> RGA0硬件框区域      |
                                                                  +-> RTSP
```

与旧路径相比，主流不再执行 `NV12 -> RGB -> CPU/同步RGA画框 -> NV12`，也不再
经过应用层主帧队列和用户态 NV12 池。主流像素在 VI、RGA、VENC 间始终由
DMA-BUF/RKMedia 硬件缓冲承载。RGA 区域配置只更新坐标和颜色，不上传整帧像素。

这里的“零拷贝”边界是视频主路径：

- RGA 与 VENC 仍会读写 DMA 内存，这是硬件处理，不是“零内存访问”。
- RockX 的旧接口要读取紧凑 BGR 子帧；代码用 CPU cache access begin/end 包围调用，
  SDK 内部是否复制不可由本项目保证。
- H.264 码流需要映射给 RTSP 库并通过网络栈发送，码流和网络层不是零拷贝。
- 若 `/tmp/.rkmedia_vi_debug` 等 SDK 调试开关启用，RKMedia 源码会强制 VI 回退 MMAP；
  做性能验收时不要启用该调试模式。

## 背压与丢帧策略

- 主视频由 RKMedia 硬件绑定直通，应用不再主动丢主帧。
- 子视频没有“最新帧覆盖”队列；池暂时耗尽时等待槽位，不主动丢当前帧。
- 两个 VENC 各用一个取码线程，避免一个通道超时阻塞另一个通道。
- 编码包进入容量固定的阻塞队列；队列满时向 VENC 取码线程施加背压，不覆盖包。
- 只有 AI 队列采用最新帧策略。AI 跟不上时淘汰过期待推理帧，不阻塞视频。
- 每 5 秒打印一次 PTS 缺口、AI 淘汰、池等待及编码包队列高水位。

典型统计如下：

```text
video main(venc=9000, pts_gap=0)
      sub(venc=9000, pts_gap=0)
      ai(source=9000, sampled=1500, completed=1500, stale_drop=0)
      osd(boxes=1, truncated=0)
      rtsp(sent=18000, queue_high_water=2/8, shutdown_abandoned=0)
```

`pts_gap=0` 是应用可观测范围内的零丢帧必要条件。断网、客户端不读或链路带宽
不足时，任何有限缓冲系统都不能无限期保证零丢帧；此时背压最终会触发停滞错误，
避免用不断增长的内存伪装稳定性。

RV1126 这代 RGA API 只提供 8 个持久区域。当前以 4 个硬件色块组成一个空心框，
所以最多同时显示 2 个框；更多框会累计到 `osd.truncated`，视频仍保持直通。若必须
显示更多框/文字，需要使用持久 ARGB 位图区域并处理区域生命周期，不能恢复整帧
CPU 绘制。

## 默认参数

| 项目 | 默认值 |
| --- | --- |
| 主流 | 1920x1080、30 FPS、H.264 Baseline CBR、8 Mbit/s、GOP 30 |
| 子流 | 640x360、30 FPS、H.264 Baseline CBR、0.8 Mbit/s、GOP 30 |
| AI | 5 FPS、置信度 0.5、结果有效期 300 ms |
| VI/RGA 硬件池 | 每通道 8 个缓冲 |
| H.264 包队列 | 8 个包，阻塞背压；正常高水位应接近 1--2 |
| RTSP | 8554，`/live/main_stream` 和 `/live/sub_stream` |

IMX415 原生能力高于 1080P；本程序请求 ISPP 的 `rkispp_scale0/1` 输出 1080P 与
360P。传感器模式、曝光和实际 30 FPS 仍由设备树、IQ 文件及 ISP 配置共同决定，
不能仅凭应用参数认定已经达到 30 FPS。

## 构建

主机核心测试：

```sh
cmake -S . -B build-host -G Ninja \
  -DRK_VISION_BUILD_DEVICE=OFF -DBUILD_TESTING=ON
cmake --build build-host
cd build-host && ctest --output-on-failure
```

RV1126 交叉编译：

```sh
cmake -S . -B build-rv1126-sdk -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/rv1126.cmake \
  -DRK_VISION_BUILD_DEVICE=ON -DRK_VISION_ENABLE_ISP=ON \
  -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-rv1126-sdk -j2
```

设备构建未显式指定类型时，CMake 也会默认使用 Release。应在详细编译输出中看到
`-O3 -DNDEBUG`。当前 SDK 和交叉工具链默认从 `/home/alientek/rv1126_sdk` 读取，
也可通过 toolchain 参数覆盖。

## 部署与运行

开发板在线后：

```sh
adb push build-rv1126-sdk/rkmedia_rockx_face_detection_rtsp /root/
adb shell chmod +x /root/rkmedia_rockx_face_detection_rtsp
adb shell 'cd /root && ./rkmedia_rockx_face_detection_rtsp'
```

如果实拍方向不对，启动时加 `--rotation 90`、`180` 或 `270`。注意 90/270 会将
1920x1080 输出变成 1080x1920；若目标必须是横向 1920x1080，优先在传感器/ISP
镜像翻转层校正方向。

常用参数：

```text
--width 1920 --height 1080 --sub-width 640 --sub-height 360
--fps 30 --ai-fps 5 --bitrate 8000000 --sub-bitrate 800000
--pool-buffers 8 --packet-queue 8 --result-ttl-ms 300
--main-node rkispp_scale0 --sub-node rkispp_scale1
--no-ai --no-osd --no-isp
```

NV12 的宽高只要求为偶数；主、子节点必须具有相同宽高比和实际视场。

## 低时延播放

推荐先用 TCP 验证稳定性：

```sh
ffplay -rtsp_transport tcp -fflags nobuffer -flags low_delay \
  -framedrop -probesize 32 -analyzeduration 0 -sync ext \
  rtsp://<开发板IP>:8554/live/main_stream
```

再使用 `ffprobe` 核对真实输出：

```sh
ffprobe -v error -rtsp_transport tcp -select_streams v:0 \
  -show_entries stream=codec_name,profile,width,height,r_frame_rate,avg_frame_rate \
  -of default=noprint_wrappers=1 \
  rtsp://<开发板IP>:8554/live/main_stream
```

VLC 默认缓存明显大于 200 ms。测试时在“打开网络串流”的高级选项中把网络缓存
设为约 100 ms，但最终延迟应使用画面时钟/LED 对拍测量，不能用播放器观感估算。

完整板端验收步骤见 [docs/board-validation.md](docs/board-validation.md)。在开发板离线
时只能证明主机测试和 ARM 编译通过，不能声称 CPU、零丢帧和端到端时延已经达标。
