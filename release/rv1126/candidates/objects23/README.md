# 23-class object model candidate

This directory contains the RV1126 INT8 model, metadata and Chinese label atlas
for the expanded 23-class detector. It is deliberately separate from
`release/rv1126/objects/`: the existing 11-class release has board validation,
whereas this candidate has passed host training, ONNX checks, RKNN Toolkit
1.7.5 conversion, an RV1126 still-image test and a short live-pipeline test.
Per-class physical-object accuracy and long-duration validation remain pending.

The first 11 class IDs are unchanged. New IDs 11-22 are person, backpack,
handbag, bowl, sandwich, chair, couch, potted plant, dining table, remote,
clock and scissors.

Upload without replacing the active release:

```bash
bash scripts/push_objects23_candidate.sh
```

Start the archived candidate full-FOV profile (4K sensor input, 1080p/360p output):

```bash
adb shell '/root/rv1126_objects23_candidate/rv1126-object23 start'
adb shell '/root/rv1126_objects23_candidate/rv1126-object23 health'
adb shell '/root/rv1126_objects23_candidate/rv1126-object23 stop'
```

This profile preserves the3840×2160 active sensor area and now shapes output to
stable29 FPS. The same model/profile is promoted to the formal `objects/`
release; this candidate copy remains only for validation history.
Native 3840×2160 main-stream output was also tested at 6 Mbps with AI/OSD off;
the board fell to roughly 53 ms per frame and the TCP stream was corrupted.
That failed mode is documented but is not provided as a default profile.

Then upload a test image and run:

```bash
adb push test.jpg /root/rv1126_objects23_candidate/test.jpg
adb shell 'cd /root/rv1126_objects23_candidate && ./object_image objects.rknn objects.json labels.png test.jpg result.jpg 0.25'
adb pull /root/rv1126_objects23_candidate/result.jpg artifacts/device-objects23-result.jpg
```

The verified still-image result is `artifacts/device-objects23-result.jpg`.
The model detected person, chair, laptop, monitor/TV and cup. Three consecutive
board runs averaged 21.893 ms total preprocess, inference and postprocess time.
Concurrent main and sub streams in the 1080p-sensor profile each decoded 900
frames in 30 seconds without errors. Keep this package separate until
physical-object accuracy and long-duration tests also pass.
