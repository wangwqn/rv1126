#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
remote_dir="/root/rv1126_objects23_candidate"

files=(
  "deploy/pipeline/build-rv1126-sdk/object_image"
  "deploy/pipeline/build-rv1126-sdk/rkmedia_object_detection_rtsp"
  "artifacts/objects23-rv1126.rknn"
  "artifacts/objects23-rv1126.json"
  "artifacts/labels23.png"
)

for relative_path in "${files[@]}"; do
  if [[ ! -s "$project_dir/$relative_path" ]]; then
    echo "Missing candidate file: $relative_path" >&2
    exit 1
  fi
done

adb shell "mkdir -p '$remote_dir'"
adb push "$project_dir/${files[0]}" "$remote_dir/object_image"
adb push "$project_dir/${files[1]}" "$remote_dir/rkmedia_object_detection_rtsp"
adb push "$project_dir/${files[2]}" "$remote_dir/objects.rknn"
adb push "$project_dir/${files[3]}" "$remote_dir/objects.json"
adb push "$project_dir/${files[4]}" "$remote_dir/labels.png"
adb shell "chmod 0755 '$remote_dir/object_image' '$remote_dir/rkmedia_object_detection_rtsp'"

echo "Uploaded 23-class candidate to $remote_dir; the validated service was not changed."
echo "Run a still-image check before replacing the active 11-class release."
