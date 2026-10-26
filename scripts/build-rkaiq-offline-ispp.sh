#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
sdk_root=${RV1126_SDK_ROOT:-/home/alientek/rv1126_sdk}
source_dir="$sdk_root/external/camera_engine_rkaiq"
build_dir="$sdk_root/buildroot/output/alientek_rv1126/build/camera_engine_rkaiq-1.0"
patch_file="$project_root/patches/rkaiq/0001-rv1126-independent-ispp-target.patch"
output_dir="$project_root/release/rv1126/lib"

if [[ ! -f "$source_dir/hwi/isp20/CamHwIsp20.cpp" ||
      ! -f "$build_dir/hwi/isp20/CamHwIsp20.cpp" ]]; then
  echo "ERROR: RV1126 RKAiq source/build tree not found under $sdk_root" >&2
  exit 1
fi

apply_once() {
  local directory=$1
  if patch --dry-run --forward -d "$directory" -p1 <"$patch_file" >/dev/null 2>&1; then
    patch --forward -d "$directory" -p1 <"$patch_file"
  elif patch --dry-run --reverse -d "$directory" -p1 <"$patch_file" >/dev/null 2>&1; then
    echo "RKAiq patch already applied in $directory"
  else
    echo "ERROR: RKAiq patch does not apply cleanly in $directory" >&2
    exit 1
  fi
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
sha256sum "$output_dir/librkaiq-offline-ispp.so"
echo "Built $output_dir/librkaiq-offline-ispp.so"
