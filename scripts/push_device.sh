#!/usr/bin/env bash
# Upload only; never stop/replace an existing service automatically.
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
source "$project_dir/scripts/lib/atomic_adb_deploy.sh"
build_dir="deploy/pipeline/build-rv1126-sdk"
for path in "$build_dir/object_image" "$build_dir/rkmedia_object_detection_rtsp" \
  release/rv1126/objects/objects.rknn release/rv1126/objects/objects.json \
  release/rv1126/objects/labels.png release/rv1126/objects/rv1126-object \
  release/rv1126/objects/rv1126-web release/rv1126/web/rv1126_web.py \
  release/rv1126/web/web-index.html release/rv1126/web/web-app.js \
  release/rv1126/web/web-styles.css; do
    test -s "$path" || { echo "Missing artifact: $path" >&2; exit 1; }
done
version=$(tr -d '[:space:]' <VERSION)
commit_file=$(mktemp /tmp/rv1126-git-commit.XXXXXX)
trap 'rm -f "$commit_file"' EXIT
git rev-parse HEAD >"$commit_file"
atomic_adb_deploy /root/rv1126_objects "build-v$version" \
  "VERSION|VERSION|0644" \
  "$commit_file|GIT_COMMIT|0644" \
  "release/rv1126/objects/objects.rknn|objects.rknn|0644" \
  "release/rv1126/objects/objects.json|objects.json|0644" \
  "release/rv1126/objects/labels.png|labels.png|0644" \
  "$build_dir/object_image|object_image|0755" \
  "release/rv1126/objects/rv1126-object|rv1126-object|0755" \
  "release/rv1126/objects/rv1126-web|rv1126-web|0755" \
  "release/rv1126/web/rv1126_web.py|rv1126-web.py|0755" \
  "release/rv1126/web/web-index.html|web-index.html|0644" \
  "release/rv1126/web/web-app.js|web-app.js|0644" \
  "release/rv1126/web/web-styles.css|web-styles.css|0644" \
  "$build_dir/rkmedia_object_detection_rtsp|rkmedia_object_detection_rtsp|0755"
echo 'Uploaded atomically. Test object_image before restarting the RTSP process.'
