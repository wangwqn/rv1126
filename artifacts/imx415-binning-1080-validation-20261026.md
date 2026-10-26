# IMX415 全视场 2x2 binning 1080p@30 验证

日期：2026-10-26

## 结论

RV1126 板载 IMX415 驱动已有可用的 2x2 相邻像素 binning 模式，无需增加新的
sensor register table。项目参数 `--sensor-mode 1080p` 会选择该模式：

```text
IMX415 3864x2192 感光阵列
  -> 2x2 adjacent-pixel binning
  -> 1944x1097 RAW12
  -> selection: left=12 top=8 width=1920 height=1080
  -> RKISP FBC420 1920x1080
  -> RKISPP TNR 2to1 + NR + SHARP
  -> scale0 1920x1080 + scale1 640x360
  -> H.264 RTSP 30 FPS
```

Sony 的 IMX415 产品资料将 3840x2160 all-pixel 和 1920x1080 2x2 adjacent
pixel binning 列为同一推荐记录区域。驱动输出的额外边缘像素用于对齐；标准
1920x1080 selection 并不是从 4K 中央截取一块 1920x1080。

## 驱动证据

板载内核源文件 `drivers/media/i2c/imx415.c` 包含：

- `BRL_ALL=2228` 和 `BRL_BINNING=1115`；
- `imx415_linear_12bit_1932x1096_594M_regs`；
- binning 模式寄存器 `0x3020/0x3021/0x3022 = 1`；
- `1944x1097 RAW12 @ 30 FPS` mode entry；
- 对应 `1920x1080` 的 crop-bounds selection。

板端运行时读取结果：

```text
Width/Height : 1944/1097
Mediabus Code: MEDIA_BUS_FMT_SGBRG12_1X12
Selection    : Left 12, Top 8, Width 1920, Height 1080
```

## 稳态帧率

测试关闭 AI、OSD 和音频，仅验证视频前端、双路编码和 RTSP：

```text
30 秒 scale0: before=673 after=1573 delta=900
average fps: 30.000
ErrCnt: 0
```

同一时刻 RKISPP 状态：

```text
Input  FBC420 1920x1080
scale0 NV12   1920x1080
scale1 NV12    640x360
TNR ON, mode 2to1, about 3 ms
NR ON
SHARP ON
Monitor Cnt: 0
```

VENC 主、子流每 5 秒均增加约 150 帧。启动阶段记录到 3 个 PTS gap，之后没有
继续增长。FFmpeg 通过 RTSP/TCP 解码主流 20 秒得到 600 帧，`dup_frames=0`、
`drop_frames=0`。

## 视场对照

在摄像头和场景不移动的条件下，分别抓取：

1. 1944x1097 RAW12 binning -> 1080p；
2. 3864x2192 RAW10 -> RKISPP scale0 1080p。

左侧摆件、显示器四边、前景瓶子和桌面边界基本重合，没有出现中心区域放大的
裁剪现象。两种模式的主要区别是曝光、锐度、噪声和 ALSC 标定，不是视场范围。

## Wi-Fi RTSP

开发板连接 Windows 热点 `wang` 后地址为 `192.168.137.56`，到网关三次 ping
均成功，信号约 -31 dBm。已移除 ADB 端口转发，宿主机可直接播放：

```text
rtsp://192.168.137.56:8554/live/main_stream
rtsp://192.168.137.56:8554/live/sub_stream
```

## 完整功能复测

随后将正式 v1.8.8 以无参数方式重启，AI、OSD、音频、TNR、NR和
SHARP全部开启。默认命令行确认为 `--sensor-mode 1080p --fps 30
--ai-fps 3 --max-osd-boxes 8`。真机结果：

```text
health: 3 s main_delta=91/92, sub_delta=92
AI: source=15 sampled=15 completed=15 stale_drop=0
OSD: boxes=0..6 truncated=0（随画面检测结果变化）
audio: captured=250 sent=250 queue_drop=0 recoveries=0
RTSP queue high water: 2/8
RKISPP ErrCnt: 0
```

通过 `wang` 热点直连 RTSP（未使用 ADB forward），`ffprobe` 确认主流包含
H.264 1080×1920（旋转270度）、30/1 FPS视频和8 kHz单声道PCMA音频。

v1.8.8 将每个框由4个RGA区域改为1个局部透明ARGB位图区域，因此
可显示最多8个“四边+中文名称+置信度”框。10秒Wi-Fi RTSP录制
共288帧，解码画面已肉眼确认多框同时叠加，日志最高记录 `osd(boxes=6,
truncated=0)`。验证帧保存为 `device-v188-multibox-live.png`，可见显示器/电视、
杯子/瓶子、鼠标和键盘等多个中文框。同一容器被同时识别成杯子和瓶子
属于模型跨类别误检，需补充数据训练，不是OSD丢框。

## 当前限制

- 板载 IQ 文件没有该 1920x1080 模式的镜头阴影标定，程序暂时旁路 ALSC；正式
  产品仍应对当前镜头制作对应 IQ 标定。
- 完整功能已完成短测，Wi-Fi双流30分钟/24小时长稳仍待执行。
- 正式管理脚本已切换到 binning 1080p@30，但未自动安装开机自启。
