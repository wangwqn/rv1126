#!/usr/bin/env bash
set -euo pipefail

remote_dir=/root/rv1126_objects
controller="$remote_dir/rv1126-object"
current_manifest="$remote_dir/DEPLOYED_SHA256SUMS"
previous_manifest="$remote_dir/DEPLOYED_SHA256SUMS.previous"

if [[ "$(adb get-state 2>/dev/null || true)" != device ]]; then
  echo "No online ADB device" >&2
  exit 1
fi

manifest=$(adb shell "cat '$previous_manifest'" 2>/dev/null | tr -d '\r')
if [[ -z "$manifest" ]]; then
  echo "No verified previous deployment is available" >&2
  exit 1
fi

files=()
while read -r hash name extra; do
  [[ -z "${hash:-}" ]] && continue
  if [[ ! "$hash" =~ ^[0-9a-f]{64}$ || ! "$name" =~ ^[A-Za-z0-9._-]+$ || -n "${extra:-}" ]]; then
    echo "Unsafe previous manifest entry: $hash $name ${extra:-}" >&2
    exit 1
  fi
  files+=("$name")
done <<<"$manifest"

if (( ${#files[@]} == 0 )); then
  echo "Previous deployment manifest is empty" >&2
  exit 1
fi

echo "Verifying previous deployment before stopping the service..."
adb shell "cd '$remote_dir' && awk '{print \$1 \"  \" \$2 \".previous\"}' DEPLOYED_SHA256SUMS.previous | sha256sum -c -"

adb shell "'$controller' stop"

swap_dir="$remote_dir/.rollback-swap-$$"
commit="set -e; target='$remote_dir'; swap='$swap_dir'; test -e '$current_manifest'; mkdir \"\$swap\";"
for name in "${files[@]}"; do
  commit+=" test -e \"\$target/$name\"; test -e \"\$target/$name.previous\";"
  commit+=" mv \"\$target/$name\" \"\$swap/$name\";"
  commit+=" mv \"\$target/$name.previous\" \"\$target/$name\";"
  commit+=" mv \"\$swap/$name\" \"\$target/$name.previous\";"
done
commit+=" mv '$current_manifest' \"\$swap/manifest\";"
commit+=" mv '$previous_manifest' '$current_manifest';"
commit+=" mv \"\$swap/manifest\" '$previous_manifest'; rmdir \"\$swap\"; sync"

adb shell "$commit"
adb shell "cd '$remote_dir' && sha256sum -c DEPLOYED_SHA256SUMS"
adb shell "'$controller' start"
healthy=0
for attempt in $(seq 1 10); do
  if adb shell "'$controller' health"; then
    healthy=1
    break
  fi
  echo "Waiting for rolled-back camera pipeline readiness ($attempt/10)..."
  sleep 2
done
if (( ! healthy )); then
  echo "Rollback files were restored, but the service did not become healthy" >&2
  exit 1
fi
echo "Rollback complete. Running this command again toggles back to the deployment now stored as .previous."
