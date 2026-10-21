#!/usr/bin/env bash
set -uo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: $0 <board-ip> [duration-seconds]" >&2
  exit 2
fi

board_ip="$1"
duration="${2:-60}"
if [[ ! "$duration" =~ ^[1-9][0-9]*$ ]]; then
  echo "Duration must be a positive integer" >&2
  exit 2
fi
if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "ffmpeg is required" >&2
  exit 2
fi

log_dir="$(mktemp -d /tmp/rv1126-udp-test.XXXXXX)"
trap 'rm -rf "$log_dir"' EXIT

run_stream() {
  local stream="$1"
  ffmpeg -hide_banner -loglevel warning -nostats -progress pipe:1 \
    -rtsp_transport udp -buffer_size 2097152 \
    -reorder_queue_size 512 -max_delay 200000 -err_detect explode \
    -i "rtsp://${board_ip}:8554/live/${stream}_stream" \
    -t "$duration" -an -f null - >"$log_dir/$stream.log" 2>&1
}

run_stream main &
main_pid=$!
run_stream sub &
sub_pid=$!

wait "$main_pid"
main_rc=$?
wait "$sub_pid"
sub_rc=$?

failure_pattern='RTP: missed|max delay reached|Invalid data|corrupt|Connection refused|timed out|error while decoding'
failed=0
for stream in main sub; do
  echo "--- $stream ---"
  grep -E 'frame=|fps=|out_time=|progress=' "$log_dir/$stream.log" | tail -12 || true
  if grep -Eqi "$failure_pattern" "$log_dir/$stream.log"; then
    echo "$stream UDP stream reported packet, protocol or decode errors:" >&2
    grep -Ei "$failure_pattern" "$log_dir/$stream.log" | tail -20 >&2
    failed=1
  fi
done

if (( main_rc != 0 || sub_rc != 0 )); then
  echo "FFmpeg exit codes: main=$main_rc sub=$sub_rc" >&2
  failed=1
fi
if (( failed != 0 )); then
  echo "UDP dual-stream test failed" >&2
  exit 1
fi

echo "UDP dual-stream test passed: duration=${duration}s, main_rc=$main_rc, sub_rc=$sub_rc"
