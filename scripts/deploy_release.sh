#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
release_dir="$project_dir/release/rv1126"

(
  cd "$release_dir"
  sha256sum -c SHA256SUMS
)

adb get-state
adb shell 'mkdir -p /root/rv1126_objects'
adb push "$release_dir/objects/object_image" \
  "$release_dir/objects/rkmedia_object_detection_rtsp" \
  "$release_dir/objects/objects.rknn" \
  "$release_dir/objects/objects.json" \
  "$release_dir/objects/labels.png" \
  /root/rv1126_objects/
adb shell 'chmod +x /root/rv1126_objects/object_image /root/rv1126_objects/rkmedia_object_detection_rtsp'

echo "Upload complete. Start it on the board with:"
echo "  cd /root/rv1126_objects"
echo "  ./rkmedia_object_detection_rtsp --rotation 270 --threshold 0.35"
