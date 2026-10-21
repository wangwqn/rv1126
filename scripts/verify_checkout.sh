#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
required=(
  "15_rkmedia_rockx_face_detection_rtsp/CMakeLists.txt"
  "third_party/yolov5/train.py"
  "datasets/coco_objects/selection.json"
  "datasets/coco_objects/manifest.json"
  "runs/objects11_refine/weights/best.pt"
  "artifacts/objects.onnx"
  "artifacts/objects-rv1126.rknn"
  "release/rv1126/SHA256SUMS"
)

for relative_path in "${required[@]}"; do
  if [[ ! -s "$project_dir/$relative_path" ]]; then
    echo "Missing required project file: $relative_path" >&2
    exit 1
  fi
done

(
  cd "$project_dir/release/rv1126"
  sha256sum -c SHA256SUMS
)

echo "Checkout is complete: source, dataset manifests, training outputs, models and RV1126 release files verified."
echo "COCO images/labels are intentionally excluded; regenerate them with scripts/prepare_data.py."
