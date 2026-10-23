#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
release_dir="$project_dir/release/rv1126"
source "$project_dir/scripts/lib/atomic_adb_deploy.sh"

(
  cd "$release_dir"
  sha256sum -c SHA256SUMS
)

version=$(tr -d '[:space:]' <"$release_dir/VERSION")
atomic_adb_deploy /root/rv1126_objects "v$version" \
  "$release_dir/VERSION|VERSION|0644" \
  "$release_dir/objects/objects.rknn|objects.rknn|0644" \
  "$release_dir/objects/objects.json|objects.json|0644" \
  "$release_dir/objects/labels.png|labels.png|0644" \
  "$release_dir/objects/object_image|object_image|0755" \
  "$release_dir/objects/rv1126-object|rv1126-object|0755" \
  "$release_dir/objects/rkmedia_object_detection_rtsp|rkmedia_object_detection_rtsp|0755"

echo "Upload complete. Manage it on the board with:"
echo "  /root/rv1126_objects/rv1126-object start"
echo "  /root/rv1126_objects/rv1126-object status"
