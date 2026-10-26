#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
candidate_dir="$project_dir/release/rv1126/candidates/full-fov-ispp1080"
source "$project_dir/scripts/lib/atomic_adb_deploy.sh"

(
  cd "$candidate_dir"
  sha256sum -c SHA256SUMS
)

for path in \
  "$candidate_dir/rkmedia_object_detection_rtsp" \
  "$candidate_dir/librkaiq.so" \
  "$candidate_dir/objects.rknn" \
  "$candidate_dir/objects.json" \
  "$candidate_dir/labels.png" \
  "$candidate_dir/VERSION" \
  "$candidate_dir/rv1126-full-fov"; do
  test -s "$path" || { echo "Missing artifact: $path" >&2; exit 1; }
done

readelf -Ws "$candidate_dir/librkaiq.so" |
  grep 'rk_aiq_ispp_target_override_version' >/dev/null

atomic_adb_deploy /root/rv1126_full_fov_candidate full-fov-ispp1080 \
  "$candidate_dir/rkmedia_object_detection_rtsp|rkmedia_object_detection_rtsp|0755" \
  "$candidate_dir/librkaiq.so|librkaiq.so|0755" \
  "$candidate_dir/objects.rknn|objects.rknn|0644" \
  "$candidate_dir/objects.json|objects.json|0644" \
  "$candidate_dir/labels.png|labels.png|0644" \
  "$candidate_dir/VERSION|VERSION|0644" \
  "$candidate_dir/rv1126-full-fov|rv1126-full-fov|0755" \
  "$candidate_dir/README.md|README.md|0644"

echo "Candidate uploaded without changing /root/rv1126_objects."
echo "First test:"
echo "  adb shell '/root/rv1126_full_fov_candidate/rv1126-full-fov start --no-ai --no-osd --no-audio --sensor-mode 4k-ispp-1080 --fps 30'"
