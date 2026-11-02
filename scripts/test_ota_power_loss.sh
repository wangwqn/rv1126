#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage:" >&2
  echo "  $0 arm BUNDLE.rvota --confirm-physical-power-test" >&2
  echo "  $0 verify" >&2
  exit 2
}

(( $# >= 1 )) || usage
mode=$1
shift
manager=/root/rv1126_ota/rv1126_ota.py
state=/userdata/rv1126-ota

[[ "$(adb get-state 2>/dev/null || true)" == device ]] || {
  echo "No online ADB device" >&2
  exit 1
}

if [[ "$mode" == arm ]]; then
  (( $# == 2 )) || usage
  bundle=$(realpath "$1")
  [[ -f "$bundle" ]] || { echo "Bundle not found: $bundle" >&2; exit 1; }
  [[ "$2" == --confirm-physical-power-test ]] || usage
  remote="$state/incoming/power-loss-test.rvota"
  echo "WARNING: this test deliberately cuts power during an A/B switch."
  echo "A serial/Loader recovery path and a verified previous release are required."
  adb shell "test -L /root/rv1126_objects && test -x '$manager'"
  adb push "$bundle" "$remote"
  adb shell "/usr/bin/python3 '$manager' stage '$remote'"
  adb shell "set -e; : >'$state/allow-power-cut-test'; RV1126_OTA_POWER_CUT_HOLD_SECONDS=120 nohup /usr/bin/python3 '$manager' apply >/tmp/rv1126-power-cut-apply.log 2>&1 </dev/null &"
  echo "Waiting until candidate symlink is switched and transaction is validating..."
  for _index in $(seq 1 150); do
    status=$(adb shell "sed -n 's/.*\"status\": \"\([^\"]*\)\".*/\1/p' '$state/transaction.json' 2>/dev/null" | tr -d '\r')
    if [[ "$status" == validating ]]; then
      echo "CUT POWER NOW. Wait at least 10 seconds, then power the board on."
      echo "After it reconnects run: $0 verify"
      exit 0
    fi
    sleep 0.2
  done
  echo "OTA did not reach the validating hold; do not cut power." >&2
  adb shell "rm -f '$state/allow-power-cut-test'" || true
  exit 1
elif [[ "$mode" == verify ]]; then
  adb shell "cat '$state/transaction.json'; echo; cat '$state/active.json'; echo; '$manager' status; /root/rv1126_objects/rv1126-object health; /root/rv1126_objects/rv1126-web health"
  status=$(adb shell "sed -n 's/.*\"status\": \"\([^\"]*\)\".*/\1/p' '$state/transaction.json' 2>/dev/null" | tr -d '\r')
  [[ "$status" == power_loss_rolled_back ]] || {
    echo "FAIL: expected power_loss_rolled_back, got: $status" >&2
    exit 1
  }
  echo "PASS: boot recovery rolled back the interrupted OTA transaction."
else
  usage
fi
