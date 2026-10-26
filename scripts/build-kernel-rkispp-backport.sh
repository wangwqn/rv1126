#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
sdk_root=${RV1126_SDK_ROOT:-/home/alientek/rv1126_sdk}
kernel_root="$sdk_root/kernel"
patch_file="$repo_root/patches/kernel/0001-rkispp-v1.7-buffer-and-tnr-first-frame-fixes.patch"
output_dir="$repo_root/release/rv1126/candidates/full-fov-ispp1080/kernel"
action=${1:-status}

test -f "$kernel_root/drivers/media/platform/rockchip/ispp/stream.c"
test -f "$patch_file"

patch_state() {
  if (cd "$kernel_root" && git apply --check "$patch_file" 2>/dev/null); then
    echo not-applied
  elif (cd "$kernel_root" && git apply --reverse --check "$patch_file" 2>/dev/null); then
    echo applied
  else
    echo divergent
  fi
}

apply_patch() {
  case "$(patch_state)" in
    not-applied)
      (cd "$kernel_root" && git apply "$patch_file")
      echo "Applied RKISPP backport to $kernel_root"
      ;;
    applied)
      echo "RKISPP backport is already applied"
      ;;
    *)
      echo "Kernel source differs from both supported states; refusing to modify it" >&2
      exit 1
      ;;
  esac
}

case "$action" in
  status)
    echo "RKISPP backport state: $(patch_state)"
    ;;
  apply)
    apply_patch
    ;;
  revert)
    case "$(patch_state)" in
      applied)
        (cd "$kernel_root" && git apply --reverse "$patch_file")
        echo "Reverted RKISPP backport from $kernel_root"
        ;;
      not-applied)
        echo "RKISPP backport is not applied"
        ;;
      *)
        echo "Kernel source is divergent; refusing automatic reverse patch" >&2
        exit 1
        ;;
    esac
    ;;
  build)
    apply_patch
    (cd "$sdk_root" && ./build.sh kernel)
    mkdir -p "$output_dir"
    install -m 0644 "$kernel_root/zboot.img" "$output_dir/zboot-rkispp-backport.img"
    (cd "$output_dir" && sha256sum zboot-rkispp-backport.img > SHA256SUMS)
    echo "Built candidate only; it has NOT been flashed:"
    echo "$output_dir/zboot-rkispp-backport.img"
    ;;
  *)
    echo "Usage: $0 {status|apply|revert|build}" >&2
    exit 2
    ;;
esac
