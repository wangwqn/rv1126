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
  "scripts/test_rtsp_udp.sh"
  "scripts/test_rtsp_av.sh"
  "scripts/test_8h_acceptance.sh"
  "scripts/lib/atomic_adb_deploy.sh"
  "scripts/deploy_release.sh"
  "scripts/rollback_device.sh"
  "15_rkmedia_rockx_face_detection_rtsp/CMakeLists.txt"
  "deploy/pipeline/CMakeLists.txt"
  "deploy/pipeline/include/vision/config.hpp"
  "deploy/pipeline/include/vision/audio.hpp"
  "deploy/objects.cpp"
  "config/classes.json"
  "config/classes-v2.json"
  "config/hyp.objects23-finetune.yaml"
  "third_party/yolov5/train.py"
  "datasets/coco_objects/selection.json"
  "datasets/coco_objects/manifest.json"
  "datasets/coco_objects_v2/selection.json"
  "datasets/coco_objects_v2/manifest.json"
  "runs/objects11_refine/weights/best.pt"
  "runs/objects23_finetune/results.csv"
  "runs/objects23_finetune/weights/best.pt"
  "artifacts/objects.onnx"
  "artifacts/objects-rv1126.rknn"
  "artifacts/objects23.onnx"
  "artifacts/objects23-rv1126.rknn"
  "artifacts/objects23-rv1126.json"
  "artifacts/labels23.png"
  "artifacts/device-objects23-result.jpg"
  "artifacts/device-objects23-live.jpg"
  "artifacts/board-validation-objects23-20261024.md"
  "artifacts/full-fov-validation-20261024.md"
  "artifacts/native-4k-output-validation-20261024.md"
  "artifacts/full-fov-29fps-validation-20261024.md"
  "artifacts/ai-rga-rate-limit-validation-20261025.md"
  "artifacts/imx415-binning-1080-validation-20261026.md"
  "artifacts/board-validation-v193-watchdog-20261027.md"
  "artifacts/device-v188-multibox-live.png"
  "artifacts/device-objects23-full-fov-4k-to-1080.jpg"
  "artifacts/device-objects23-full-fov-live-20261024.jpg"
  "scripts/push_objects23_candidate.sh"
  "release/rv1126/SHA256SUMS"
  "release/rv1126/VERSION"
  "release/rv1126/objects/rv1126-object"
  "release/rv1126/candidates/objects23/SHA256SUMS"
  "release/rv1126/candidates/objects23/rv1126-object23"
  "release/rv1126/candidates/objects23/VERSION"
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
release_version="$(tr -d '[:space:]' < "$project_dir/release/rv1126/VERSION")"
if [[ "$release_version" != "$version" ]]; then
  echo "Release VERSION differs from project VERSION: $release_version != $version" >&2
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

(
  cd "$project_dir/release/rv1126/candidates/objects23"
  sha256sum -c SHA256SUMS
)

echo "Checkout v$version is complete: source, dataset manifests, training outputs, models and RV1126 release files verified."
echo "COCO images/labels are intentionally excluded; regenerate either dataset with scripts/prepare_data.py and its matching --classes/--output options."
