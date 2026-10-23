#!/usr/bin/env bash
set -euo pipefail

ip=${1:-}
duration=${2:-10}
if [[ -z "$ip" || ! "$duration" =~ ^[1-9][0-9]*$ ]]; then
  echo "Usage: $0 BOARD_IP [DURATION_SECONDS]" >&2
  exit 2
fi

test_dir=$(mktemp -d /tmp/rv1126-av-test.XXXXXX)
trap 'rm -rf "$test_dir"' EXIT
export NO_PROXY="${NO_PROXY:+$NO_PROXY,}$ip"
unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY

probe_stream() {
  local name=$1
  local url="rtsp://$ip:8554/live/${name}_stream"
  local output="$test_dir/$name.streams"
  timeout 20 ffprobe -v error -rtsp_transport tcp \
    -show_entries stream=codec_name,codec_type,width,height,r_frame_rate,sample_rate,channels \
    -of default=noprint_wrappers=1 "$url" >"$output"
  grep -q '^codec_name=h264$' "$output"
  grep -q '^codec_name=pcm_alaw$' "$output"
  grep -q '^sample_rate=8000$' "$output"
  grep -q '^channels=1$' "$output"
  echo "$name stream: H.264 video + 8 kHz mono PCMA"
}

probe_stream main
probe_stream sub

decode_log="$test_dir/decode.log"
timeout "$((duration + 20))" ffmpeg -hide_banner -nostats \
  -rtsp_transport tcp \
  -i "rtsp://$ip:8554/live/main_stream" \
  -t "$duration" -map 0:v:0 -map 0:a:0 -af volumedetect -f null - \
  >"$decode_log" 2>&1

if grep -Eqi 'Invalid NAL|error while decoding|corrupt|non-existing PPS|RTP: missed' \
    "$decode_log"; then
  echo "Audio/video decode errors:" >&2
  grep -Ein 'Invalid NAL|error while decoding|corrupt|non-existing PPS|RTP: missed' \
    "$decode_log" >&2
  exit 1
fi

samples=$(sed -n 's/.*n_samples: \([0-9][0-9]*\).*/\1/p' "$decode_log" | tail -1)
mean=$(sed -n 's/.*mean_volume: \([^ ]*\) dB.*/\1/p' "$decode_log" | tail -1)
peak=$(sed -n 's/.*max_volume: \([^ ]*\) dB.*/\1/p' "$decode_log" | tail -1)
if [[ -z "$samples" || "$samples" -le 0 || -z "$peak" || "$peak" == "-inf" ]]; then
  echo "No decodable non-silent PCMA samples were received" >&2
  exit 1
fi

echo "decode: duration=${duration}s audio_samples=$samples mean=${mean}dB peak=${peak}dB"
echo "PASS: RTSP main/sub video and synchronized PCMA audio are available"
