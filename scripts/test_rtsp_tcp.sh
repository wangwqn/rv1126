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

if ! command -v ffprobe >/dev/null 2>&1; then
  echo "ffprobe is required: sudo apt install ffmpeg" >&2
  exit 2
fi
if ! command -v stdbuf >/dev/null 2>&1; then
  echo "stdbuf is required (provided by GNU coreutils)" >&2
  exit 2
fi

# RTSP is a local connection. Do not let desktop proxy applications redirect it.
unset http_proxy https_proxy all_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY
export no_proxy="localhost,127.0.0.1,${board_ip}"
export NO_PROXY="$no_proxy"

log_dir=/tmp/rv1126_tcp_test
mkdir -p "$log_dir"
: >"$log_dir/main.log"
: >"$log_dir/sub.log"
: >"$log_dir/main.frames"
: >"$log_dir/sub.frames"

wall_timeout=$((duration + 30))
expected=$((duration * 30))

run_stream() {
  local stream="$1"
  exec timeout --signal=TERM --kill-after=5 "$wall_timeout" \
    stdbuf -oL ffprobe -v warning -rtsp_transport tcp \
    -select_streams v:0 -show_frames \
    -show_entries frame=best_effort_timestamp \
    -of csv=p=0 \
    "rtsp://${board_ip}:8554/live/${stream}_stream" \
    >"$log_dir/$stream.frames" 2>"$log_dir/$stream.log"
}

read_frames() {
  wc -l <"$1"
}

wait_until_ready() {
  local pid="$1"
  local log="$2"
  local ignored
  for ignored in $(seq 1 100); do
    if [[ -s "$log" ]]; then
      return 0
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
      return 1
    fi
    sleep 0.1
  done
  return 1
}

echo "RV1126 RTSP/TCP dual-stream test"
echo "board=$board_ip duration=${duration}s expected=${expected} frames/stream"
echo "Starting main stream..."

run_stream main &
main_pid=$!

# The old librtsp can stall when two SETUP handshakes arrive together. Start
# the sub stream only after FFmpeg reports that main has received video frames.
if ! wait_until_ready "$main_pid" "$log_dir/main.frames"; then
  echo "FAIL: main stream did not produce a frame within 10 seconds" >&2
  kill -TERM "$main_pid" 2>/dev/null || true
  wait "$main_pid" 2>/dev/null || true
  if [[ -s "$log_dir/main.log" ]]; then
    tail -40 "$log_dir/main.log" >&2
  else
    echo "Main log is empty; RTSP handshake did not complete." >&2
  fi
  echo "log: $log_dir/main.log" >&2
  exit 1
fi

echo "Main stream is ready; starting sub stream..."
run_stream sub &
sub_pid=$!

if ! wait_until_ready "$sub_pid" "$log_dir/sub.frames"; then
  echo "FAIL: sub stream did not produce a frame within 10 seconds" >&2
  kill -TERM "$main_pid" "$sub_pid" 2>/dev/null || true
  wait "$main_pid" 2>/dev/null || true
  wait "$sub_pid" 2>/dev/null || true
  [[ -s "$log_dir/sub.log" ]] && tail -40 "$log_dir/sub.log" >&2
  echo "logs: $log_dir/main.log $log_dir/sub.log" >&2
  exit 1
fi

# Do not use ffmpeg -t here: it follows media PTS, and this old librtsp can
# jump PTS during SETUP. ffprobe emits one line per decoded frame; count line
# differences over an explicit wall-clock interval after both streams are ready.
main_start=$(read_frames "$log_dir/main.frames")
sub_start=$(read_frames "$log_dir/sub.frames")
echo "Both streams are ready; measuring ${duration} wall-clock seconds..."
sleep "$duration"

kill -TERM "$main_pid" "$sub_pid" 2>/dev/null || true
wait "$main_pid"
main_rc=$?
wait "$sub_pid"
sub_rc=$?

main_end=$(read_frames "$log_dir/main.frames")
sub_end=$(read_frames "$log_dir/sub.frames")
main_frames=$((main_end - main_start))
sub_frames=$((sub_end - sub_start))
main_fps=$(awk -v n="$main_frames" -v d="$duration" 'BEGIN {printf "%.2f", n/d}')
sub_fps=$(awk -v n="$sub_frames" -v d="$duration" 'BEGIN {printf "%.2f", n/d}')

echo "main: frames=$main_frames average_fps=$main_fps exit_code=$main_rc"
echo "sub : frames=$sub_frames average_fps=$sub_fps exit_code=$sub_rc"

failure_pattern='corrupt|error while decoding|Invalid NAL|non-existing PPS|RTP: missed|max delay reached|Invalid data|timed out|Connection refused|CSeq .*expected'
failed=0

if grep -Eqi "$failure_pattern" "$log_dir/main.log" "$log_dir/sub.log"; then
  echo "Decode or transport errors:" >&2
  grep -Ein "$failure_pattern" "$log_dir/main.log" "$log_dir/sub.log" | tail -30 >&2
  failed=1
fi

minimum=$((expected - 2))
if (( main_frames < minimum || sub_frames < minimum )); then
  failed=1
fi

if (( failed != 0 )); then
  echo "FAIL: TCP dual-stream test failed" >&2
  echo "logs: $log_dir/main.log $log_dir/sub.log" >&2
  exit 1
fi

echo "PASS: TCP dual-stream test passed"
echo "logs: $log_dir/main.log $log_dir/sub.log"
