#!/usr/bin/env bash
# Upload only; never stop/replace an existing service automatically.
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
build_dir="deploy/pipeline/build-rv1126-sdk"
for path in "$build_dir/object_image" "$build_dir/rkmedia_object_detection_rtsp" artifacts/objects-rv1126.rknn artifacts/objects-rv1126.json artifacts/labels.png; do
    test -s "$path" || { echo "Missing artifact: $path" >&2; exit 1; }
done
adb get-state
adb shell 'mkdir -p /root/rv1126_objects'
adb push "$build_dir/object_image" "$build_dir/rkmedia_object_detection_rtsp" artifacts/labels.png /root/rv1126_objects/
adb push artifacts/objects-rv1126.rknn /root/rv1126_objects/objects.rknn
adb push artifacts/objects-rv1126.json /root/rv1126_objects/objects.json
adb shell 'chmod +x /root/rv1126_objects/object_image /root/rv1126_objects/rkmedia_object_detection_rtsp'
echo 'Uploaded. Test object_image before starting the RTSP process.'
