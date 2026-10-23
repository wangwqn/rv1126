#!/usr/bin/env bash
# Upload only; never stop/replace an existing service automatically.
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
source "$project_dir/scripts/lib/atomic_adb_deploy.sh"
build_dir="deploy/pipeline/build-rv1126-sdk"
for path in "$build_dir/object_image" "$build_dir/rkmedia_object_detection_rtsp" artifacts/objects-rv1126.rknn artifacts/objects-rv1126.json artifacts/labels.png release/rv1126/objects/rv1126-object; do
    test -s "$path" || { echo "Missing artifact: $path" >&2; exit 1; }
done
version=$(tr -d '[:space:]' <VERSION)
atomic_adb_deploy /root/rv1126_objects "build-v$version" \
  "VERSION|VERSION|0644" \
  "artifacts/objects-rv1126.rknn|objects.rknn|0644" \
  "artifacts/objects-rv1126.json|objects.json|0644" \
  "artifacts/labels.png|labels.png|0644" \
  "$build_dir/object_image|object_image|0755" \
  "release/rv1126/objects/rv1126-object|rv1126-object|0755" \
  "$build_dir/rkmedia_object_detection_rtsp|rkmedia_object_detection_rtsp|0755"
echo 'Uploaded atomically. Test object_image before restarting the RTSP process.'
