# 23-class object model candidate

This directory contains the RV1126 INT8 model, metadata and Chinese label atlas
for the expanded 23-class detector. It is deliberately separate from
`release/rv1126/objects/`: the existing 11-class release has board validation,
whereas this candidate has passed host training, ONNX checks and RKNN Toolkit
1.7.5 conversion but has not yet run on the currently disconnected board.

The first 11 class IDs are unchanged. New IDs 11-22 are person, backpack,
handbag, bowl, sandwich, chair, couch, potted plant, dining table, remote,
clock and scissors.

Upload without replacing the active release:

```bash
bash scripts/push_objects23_candidate.sh
```

Then upload a test image and run:

```bash
adb push test.jpg /root/rv1126_objects23_candidate/test.jpg
adb shell 'cd /root/rv1126_objects23_candidate && ./object_image objects.rknn objects.json labels.png test.jpg result.jpg 0.25'
adb pull /root/rv1126_objects23_candidate/result.jpg artifacts/device-objects23-result.jpg
```

Only after the still-image result and live NPU pipeline pass should these three
assets replace the corresponding files in `release/rv1126/objects/`.
