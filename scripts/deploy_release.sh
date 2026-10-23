#!/usr/bin/env bash
set -euo pipefail

restart=0
if [[ "${1:-}" == --restart ]]; then
  restart=1
  shift
fi
if (( $# != 0 )); then
  echo "Usage: $0 [--restart]" >&2
  exit 2
fi

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
if (( restart )); then
  adb shell '/root/rv1126_objects/rv1126-object restart'
  healthy=0
  for attempt in $(seq 1 10); do
    if adb shell '/root/rv1126_objects/rv1126-object health'; then
      healthy=1
      break
    fi
    echo "Waiting for camera pipeline readiness ($attempt/10)..."
    sleep 2
  done
  if (( ! healthy )); then
    echo "Deployment completed, but the restarted service did not become healthy" >&2
    exit 1
  fi
fi
