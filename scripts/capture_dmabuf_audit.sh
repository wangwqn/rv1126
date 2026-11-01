#!/usr/bin/env bash
set -euo pipefail

duration=${1:-300}
output_dir=${2:-"/tmp/rv1126-dmabuf-$(date +%Y%m%d-%H%M%S)"}
[[ "$duration" =~ ^[0-9]+$ ]] && (( duration >= 10 )) || {
  echo "Usage: $0 [duration_seconds>=10] [output_directory]" >&2
  exit 2
}
[[ "$(adb get-state 2>/dev/null || true)" == device ]] || {
  echo "No online ADB device" >&2
  exit 1
}

mkdir -p "$output_dir"
csv="$output_dir/dmabuf-samples.csv"
printf '%s\n' 'host_epoch,pid,process_fds,process_bytes,global_objects,global_bytes,rss_kb,threads' >"$csv"

for ((sample = 0; sample < duration; ++sample)); do
  epoch=$(date +%s)
  line=$(adb shell '
    pid=$(cat /var/run/rv1126-object.pid 2>/dev/null || true)
    [ -n "$pid" ] && [ -d "/proc/$pid" ] || exit 3
    process_fds=0; process_bytes=0
    for info in /proc/$pid/fdinfo/*; do
      size=$(awk '\''
        $1 == "size:" { size=$2 }
        $1 == "exp_name:" { dma=1 }
        END { if (dma) print size+0 }
      '\'' "$info" 2>/dev/null) || true
      [ -n "$size" ] || continue
      process_fds=$((process_fds + 1))
      process_bytes=$((process_bytes + size))
    done
    set -- $(awk '\''
      /^[0-9]/ { count++; bytes+=$1 }
      END { print count+0, bytes+0 }
    '\'' /sys/kernel/debug/dma_buf/bufinfo 2>/dev/null)
    global_objects=$1; global_bytes=$2
    rss=$(awk '\''$1 == "VmRSS:" { print $2 }'\'' /proc/$pid/status)
    threads=$(awk '\''$1 == "Threads:" { print $2 }'\'' /proc/$pid/status)
    echo "$pid,$process_fds,$process_bytes,$global_objects,$global_bytes,${rss:-0},${threads:-0}"
  ' | tr -d '\r')
  [[ "$line" =~ ^[0-9]+,[0-9]+,[0-9]+,[0-9]+,[0-9]+,[0-9]+,[0-9]+$ ]] || {
    echo "Invalid board sample: $line" >&2
    exit 1
  }
  printf '%s,%s\n' "$epoch" "$line" >>"$csv"
  sleep 1
done

adb shell 'cat /tmp/rv1126-object.log' >"$output_dir/application.log"
grep -E '^(video |latency_ms |dma_buf |dma_wait_ms |dma_graph )' \
  "$output_dir/application.log" >"$output_dir/pipeline-summary.log" || true

awk -F, '
  NR == 2 { first_fds=$3; first_bytes=$4; first_rss=$7 }
  NR > 1 { last_fds=$3; last_bytes=$4; last_rss=$7; samples++;
           if ($3>max_fds) max_fds=$3; if ($7>max_rss) max_rss=$7 }
  END {
    printf "samples=%d process_fd_delta=%d process_bytes_delta=%d rss_delta_kb=%d max_process_fds=%d max_rss_kb=%d\n",
           samples, last_fds-first_fds, last_bytes-first_bytes,
           last_rss-first_rss, max_fds, max_rss
  }
' "$csv" | tee "$output_dir/result.txt"

echo "DMA-BUF audit saved to $output_dir"
