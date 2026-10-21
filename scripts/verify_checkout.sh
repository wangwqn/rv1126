#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
required=(
  "VERSION"
  "CHANGELOG.md"
  "README.md"
  "STATUS.md"
  "TESTING.md"
  "handoff/VERSION_CONTROL.md"
  "15_rkmedia_rockx_face_detection_rtsp/CMakeLists.txt"
  "deploy/pipeline/CMakeLists.txt"
  "deploy/pipeline/include/vision/config.hpp"
  "deploy/objects.cpp"
  "config/classes.json"
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

version="$(tr -d '[:space:]' < "$project_dir/VERSION")"
if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "Invalid VERSION: expected major.minor.patch, got '$version'" >&2
  exit 1
fi

if git -C "$project_dir" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  for relative_path in "${required[@]}"; do
    if ! git -C "$project_dir" ls-files --error-unmatch "$relative_path" >/dev/null 2>&1; then
      echo "Required project file is not tracked by Git: $relative_path" >&2
      exit 1
    fi
  done
fi

(
  cd "$project_dir/release/rv1126"
  sha256sum -c SHA256SUMS
)

echo "Checkout v$version is complete: source, dataset manifests, training outputs, models and RV1126 release files verified."
echo "COCO images/labels are intentionally excluded; regenerate them with scripts/prepare_data.py."
