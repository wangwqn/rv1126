#!/usr/bin/env bash
set -uo pipefail

duration="${1:-45}"
stamp=$(date +%Y%m%d-%H%M%S)
output_dir="${2:-/tmp/rv1126_runtime_latency_${stamp}}"
remote_dir=/root/rv1126_objects
remote_app="$remote_dir/rkmedia_object_detection_rtsp"
remote_csv=/tmp/rkvision-latency.csv
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

if [[ ! "$duration" =~ ^[1-9][0-9]*$ ]] || (( duration < 20 )); then
  echo "Usage: $0 [duration-seconds>=20] [output-directory]" >&2
  exit 2
fi
if ! command -v adb >/dev/null 2>&1; then
  echo "adb is required" >&2
  exit 2
fi
if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 is required for the SVG plot" >&2
  exit 2
fi
if [[ "$(adb get-state 2>/dev/null || true)" != "device" ]]; then
  echo "No online ADB device. Power on/reconnect the board and run: adb devices" >&2
  exit 1
fi
if [[ -n "$(adb shell 'pidof rkmedia_object_detection_rtsp' 2>/dev/null | tr -d '\r')" ]]; then
  echo "A pipeline is already running. Stop it first so its stdout can be captured." >&2
  exit 1
fi

mkdir -p "$output_dir"
adb shell 'cat /proc/rkispp0 2>/dev/null' >"$output_dir/rkispp-before.txt"

echo "Capturing ${duration}s to $output_dir"
echo "The first three seconds after graph start are warm-up and are not sampled."

set +e
timeout --signal=TERM --kill-after=15 "$((duration + 30))" \
  adb shell "cd '$remote_dir' && timeout '$duration' '$remote_app' \
    --sensor-mode 1080p --rotation 270 --threshold 0.35 \
    --latency-csv '$remote_csv'" \
  2>&1 | tee "$output_dir/application.log"
application_rc=${PIPESTATUS[0]}
set -e

adb shell 'cat /proc/rkispp0 2>/dev/null' >"$output_dir/rkispp-after.txt"
if ! adb pull "$remote_csv" "$output_dir/latency.csv" >/dev/null; then
  echo "Failed to pull $remote_csv from the board" >&2
  exit 1
fi
grep -E '^(video |latency_ms )' "$output_dir/application.log" \
  >"$output_dir/latency-summary.txt" || true
python3 "$script_dir/plot_latency_csv.py" \
  "$output_dir/latency.csv" "$output_dir/latency.svg" \
  >"$output_dir/latency-percentiles.csv"

case "$application_rc" in
  0|124|143) ;;
  *)
    echo "Capture command failed with exit code $application_rc" >&2
    exit "$application_rc"
    ;;
esac

if ! grep -q '^latency_ms ' "$output_dir/latency-summary.txt"; then
  echo "No latency samples were produced; see $output_dir/application.log" >&2
  exit 1
fi
if grep -Eq 'dropped=[1-9][0-9]*|error=1' "$output_dir/latency-summary.txt"; then
  echo "CSV writer dropped samples or reported an I/O error" >&2
  tail -2 "$output_dir/latency-summary.txt" >&2
  exit 1
fi
if (( $(wc -l <"$output_dir/latency.csv") < 2 )); then
  echo "Latency CSV has no data rows" >&2
  exit 1
fi

echo "Capture complete:"
echo "  $output_dir/latency-summary.txt"
echo "  $output_dir/latency.csv"
echo "  $output_dir/latency-percentiles.csv"
echo "  $output_dir/latency.svg"
echo "  $output_dir/rkispp-before.txt"
echo "  $output_dir/rkispp-after.txt"
