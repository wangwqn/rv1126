# IMX415 原始 4K 主码流输出验证（2026-10-24）

## 测试配置

```text
IMX415：3864×2192 RAW10，有效裁剪 3840×2160
ISPP scale0：3840×2160
RGA 旋转：270°
VENC 主流：2160×3840 H.264 Constrained Baseline，标称30 FPS，6 Mbps
子流：640×360，600 kbps
AI/OSD：关闭，排除推理和画框负载
TNR：ON，2to1
```

选择6 Mbps而不是8 Mbps，是因为板载旧 `librtsp` 的TCP发送路径在8 Mbps已有
确定的拥塞和码流损坏记录。

## 板端结果

- 传感器格式和有效裁剪均正确。
- ISPP输入3840×2160，scale0输出3840×2160，scale1输出640×360。
- TNR单帧约24 ms，scale0/scale1约53 ms/帧，管线delay约261 ms。
- 运行到主流380帧、子流380帧时，两路均已累计225个PTS缺口。
- `health` 仍能通过，因为健康检查只验证计数继续增长、端口、TNR和错误计数，
  不代表帧率达到30 FPS。

## 客户端结果

FFprobe确认主流为H.264 Constrained Baseline、2160×3840、标称30/1，但握手后立即
出现损坏宏块和解码错误。预热后10秒FFmpeg窗口只得到87个可解码帧，并记录18条
损坏/协议错误，包括 `corrupt decoded frame` 和旧 `librtsp` 的CSeq异常。

因此原始4K输出同时受板端3840×2160 TNR/ISPP/RGA/VENC吞吐和旧RTSP TCP发送路径
限制，不是单纯增加码率可以解决。测试结束后已恢复23类候选的4K传感器采集、
1920×1080/640×360输出配置。

## 结论

当前RV1126 BSP与旧 `librtsp` 组合不适合本项目的实时原始4K RTSP输出。保留完整
视场时应继续采用4K传感器采集后缩放到1080p；若必须输出4K，需要重新设计链路，
至少包括关闭/旁路4K TNR、验证RGA/VENC吞吐，并替换异步RTSP/RTP发送实现。
