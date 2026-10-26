#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
sdk_root=${RV1126_SDK_ROOT:-/home/alientek/rv1126_sdk}
source_dir="$sdk_root/external/camera_engine_rkaiq"
build_dir="$sdk_root/buildroot/output/alientek_rv1126/build/camera_engine_rkaiq-1.0"
patch_dir="$project_root/patches/rkaiq"
output_dir="$project_root/release/rv1126/lib"

if [[ ! -f "$source_dir/hwi/isp20/CamHwIsp20.cpp" ||
      ! -f "$build_dir/hwi/isp20/CamHwIsp20.cpp" ]]; then
  echo "ERROR: RV1126 RKAiq source/build tree not found under $sdk_root" >&2
  exit 1
fi

apply_once() {
  local directory=$1
  local patch_file patch_name marker
  for patch_file in "$patch_dir"/*.patch; do
    patch_name=$(basename "$patch_file")
    case "$patch_name" in
      0001-*) marker='rk_aiq_ispp_target_override_version' ;;
      0002-*) marker='rk_aiq_ispp_target_override_applied' ;;
      0003-*) marker='for (uint32_t ispp_slot = 0; ispp_slot < MAX_CAM_NUM;' ;;
      *) marker='' ;;
    esac
    if [[ -n "$marker" ]] && grep -Fq "$marker" \
        "$directory/hwi/isp20/CamHwIsp20.cpp"; then
      echo "RKAiq patch already applied in $directory: $patch_name"
    elif patch --dry-run --forward -d "$directory" -p1 <"$patch_file" >/dev/null 2>&1; then
      patch --forward -d "$directory" -p1 <"$patch_file"
    elif patch --dry-run --reverse -d "$directory" -p1 <"$patch_file" >/dev/null 2>&1; then
      echo "RKAiq patch already applied in $directory: $patch_name"
    else
      echo "ERROR: RKAiq patch does not apply cleanly in $directory: $patch_file" >&2
      exit 1
    fi
  done
}

apply_once "$source_dir"
apply_once "$build_dir"
cmake --build "$build_dir" --target rkaiq -- -j"${JOBS:-2}"

mkdir -p "$output_dir"
install -m 0755 "$build_dir/all_lib/Release/librkaiq.so" \
  "$output_dir/librkaiq-offline-ispp.so"
"$sdk_root/buildroot/output/alientek_rv1126/host/bin/arm-linux-gnueabihf-strip" \
  --strip-unneeded "$output_dir/librkaiq-offline-ispp.so"

readelf -Ws "$output_dir/librkaiq-offline-ispp.so" |
  grep 'rk_aiq_ispp_target_override_version' >/dev/null
readelf -Ws "$output_dir/librkaiq-offline-ispp.so" |
  grep 'rk_aiq_ispp_target_override_applied' >/dev/null
sha256sum "$output_dir/librkaiq-offline-ispp.so"
echo "Built $output_dir/librkaiq-offline-ispp.so"
