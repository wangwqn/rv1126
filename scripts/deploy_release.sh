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

if adb shell 'test -L /root/rv1126_objects' >/dev/null 2>&1; then
  echo "Refusing an in-place ADB deployment after A/B OTA bootstrap." >&2
  echo "Build a signed .rvota package and use the OTA verify/apply workflow." >&2
  exit 1
fi

(
  cd "$release_dir"
  sha256sum -c SHA256SUMS
)

version=$(tr -d '[:space:]' <"$release_dir/VERSION")
commit_file=$(mktemp /tmp/rv1126-git-commit.XXXXXX)
trap 'rm -f "$commit_file"' EXIT
git -C "$project_dir" rev-parse HEAD >"$commit_file"
atomic_adb_deploy /root/rv1126_objects "v$version" \
  "$release_dir/VERSION|VERSION|0644" \
  "$commit_file|GIT_COMMIT|0644" \
  "$release_dir/objects/objects.rknn|objects.rknn|0644" \
  "$release_dir/objects/objects.json|objects.json|0644" \
  "$release_dir/objects/labels.png|labels.png|0644" \
  "$release_dir/objects/object_image|object_image|0755" \
  "$release_dir/objects/rv1126-object|rv1126-object|0755" \
  "$release_dir/objects/rv1126-web|rv1126-web|0755" \
  "$release_dir/web/rv1126_web.py|rv1126-web.py|0755" \
  "$release_dir/web/web-index.html|web-index.html|0644" \
  "$release_dir/web/web-app.js|web-app.js|0644" \
  "$release_dir/web/web-styles.css|web-styles.css|0644" \
  "$release_dir/objects/rkmedia_object_detection_rtsp|rkmedia_object_detection_rtsp|0755"

echo "Upload complete. Manage it on the board with:"
echo "  /root/rv1126_objects/rv1126-object start"
echo "  /root/rv1126_objects/rv1126-object status"
echo "  /root/rv1126_objects/rv1126-web start"
echo "  /root/rv1126_objects/rv1126-web token"
if (( restart )); then
  adb shell '/root/rv1126_objects/rv1126-object restart'
  healthy=0
  for attempt in $(seq 1 10); do
    health_output=$(adb shell '/root/rv1126_objects/rv1126-object health' 2>&1 |
      tr -d '\r') || true
    printf '%s\n' "$health_output"
    if grep -q '^Healthy:' <<<"$health_output"; then
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
  adb shell '/root/rv1126_objects/rv1126-web restart'
  adb shell '/root/rv1126_objects/rv1126-web health'
fi
