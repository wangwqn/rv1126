# AI RGA输入限速与全视场帧率验证（2026-10-25）

## 改动

旧管线把640×360子流的全部29/30 FPS送入RGA2转换为BGR，再在应用层只保留
3 FPS供RKNN推理。新管线调用 `RK_MPI_SYS_SetFrameRate(RK_ID_RGA, 2, ...)`，
RKMedia在 `Flow::SendInput()` 中先丢弃非采样帧，再执行RGA转换。

RKNN模型使用第一代RKNN Toolkit 1.7.5重新执行INT8转换；新输出与正式23类模型
SHA256均为 `6c63cb88dfc0ac28419d963d5270b38cfc54add317e8bcb2a3abfafdf263b824`。
RV1126不是RV1126B，不兼容RKNN-Toolkit2/RKNPU2模型。

## 29 FPS完整功能结果

配置为IMX415 4K全视场输入、1920×1080/640×360输出、TNR 2to1、AI 3 FPS、
主流4 Mbps、子流600 kbps、PCMA音频。运行统计：

```text
main(venc=1447, pts_gap=0) sub(venc=1447, pts_gap=0)
ai(source=150, sampled=150, completed=150, stale_drop=0)
rtsp(queue_high_water=2/8) audio(queue_drop=0, recoveries=0)
```

10秒采样中应用占四核总容量19.65%（约一个核心的78.60%），原全视场基线为
26.30%。健康检查为TNR ON、ISPP `ErrCnt=0`。

## 30 FPS边界

30 FPS、12个硬件缓冲和16包RTSP队列下，AI分支已准确限制为3 FPS，但25秒时
主/子流约740/741帧，PTS缺口各7个。此时CPU热区约84°C，75°C一级热阈值已
超过，devfreq cooling state为1/3和3/7，NPU实际约594/600 MHz。

因此v1.8.7保留稳定29 FPS默认值。30 FPS必须先增加散热片/风扇、把SoC稳态温度
降到75°C以下，再做至少30分钟双流验收；不得用复制帧或重写PTS宣称真实30 FPS。
