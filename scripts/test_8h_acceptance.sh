#!/usr/bin/env bash
set -uo pipefail

if [[ $# -lt 1 || $# -gt 4 ]]; then
  echo "Usage: $0 BOARD_IP [TOTAL_SECONDS] [CYCLE_SECONDS] [OUTPUT_DIR]" >&2
  exit 2
fi

board_ip=$1
total_seconds=${2:-28800}
cycle_seconds=${3:-600}
stamp=$(date +%Y%m%d-%H%M%S)
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
output_dir=${4:-/tmp/rv1126-acceptance-8h-$stamp}

for value in "$total_seconds" "$cycle_seconds"; do
  [[ "$value" =~ ^[1-9][0-9]*$ ]] || {
    echo "Durations must be positive integers" >&2
    exit 2
  }
done
(( total_seconds >= 60 )) || { echo "TOTAL_SECONDS must be at least 60" >&2; exit 2; }
(( cycle_seconds >= 60 )) || { echo "CYCLE_SECONDS must be at least 60" >&2; exit 2; }

for command in adb ffmpeg ffprobe ping python3; do
  command -v "$command" >/dev/null 2>&1 || {
    echo "Missing required command: $command" >&2
    exit 2
  }
done

mkdir -p "$output_dir/rtsp" "$output_dir/av" "$output_dir/failures"
stop_file="$output_dir/telemetry.stop"
rm -f "$stop_file"

start_epoch=$(date +%s)
end_epoch=$((start_epoch + total_seconds))
start_iso=$(date -Is)
git_commit=$(git -C "$project_dir" rev-parse HEAD 2>/dev/null || echo unknown)

unset http_proxy https_proxy all_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY
export no_proxy="localhost,127.0.0.1,$board_ip"
export NO_PROXY="$no_proxy"

{
  echo "start=$start_iso"
  echo "board_ip=$board_ip"
  echo "total_seconds=$total_seconds"
  echo "cycle_seconds=$cycle_seconds"
  echo "git_commit=$git_commit"
  adb devices -l
  adb shell 'cat /root/rv1126_objects/VERSION 2>/dev/null; /root/rv1126_objects/rv1126-object status 2>&1 || true'
} >"$output_dir/metadata.txt" 2>&1

adb shell 'cat /proc/rkispp0 2>/dev/null' >"$output_dir/rkispp-before.txt" 2>&1 || true
adb shell 'dmesg | tail -1000' >"$output_dir/dmesg-before.txt" 2>&1 || true

telemetry_loop() {
  echo "host_time,host_network_reachable,pid,cpu_total_percent,rss_kb,hwm_kb,threads,fd_count,cpu_temp_mc,npu_temp_mc,restarts,errcnt,main_venc,sub_venc,ai_completed,osd_boxes,audio_sent" \
    >"$output_dir/resources.csv"

  while [[ ! -e "$stop_file" && $(date +%s) -lt $end_epoch ]]; do
    host_time=$(date -Is)
    host_network_reachable=0
    if ping -c 3 -W 1 "$board_ip" >/dev/null 2>&1; then
      host_network_reachable=1
    fi
    row=$(adb shell '
      pid=$(cat /var/run/rv1126-object.pid 2>/dev/null)
      if [ -z "$pid" ] || [ ! -r "/proc/$pid/stat" ]; then
        echo "0,NA,0,0,0,0,0,0,0,-1,0,0,0,0,0"
        exit
      fi

      p1=$(awk "{print \$14+\$15}" "/proc/$pid/stat")
      t1=$(awk "/^cpu / {s=0; for(i=2;i<=NF;i++) s+=\$i; print s}" /proc/stat)
      sleep 5
      if [ ! -r "/proc/$pid/stat" ]; then
        echo "0,NA,0,0,0,0,0,0,0,-1,0,0,0,0,0"
        exit
      fi
      p2=$(awk "{print \$14+\$15}" "/proc/$pid/stat")
      t2=$(awk "/^cpu / {s=0; for(i=2;i<=NF;i++) s+=\$i; print s}" /proc/stat)
      cpu=$(awk -v p1="$p1" -v p2="$p2" -v t1="$t1" -v t2="$t2" \
        "BEGIN {if(t2>t1) printf \"%.2f\",100*(p2-p1)/(t2-t1); else printf \"0.00\"}")
      rss=$(awk "/^VmRSS:/ {print \$2}" "/proc/$pid/status")
      hwm=$(awk "/^VmHWM:/ {print \$2}" "/proc/$pid/status")
      threads=$(awk "/^Threads:/ {print \$2}" "/proc/$pid/status")
      fds=$(ls "/proc/$pid/fd" 2>/dev/null | wc -l)

      cpu_temp=0
      npu_temp=0
      for zone in /sys/class/thermal/thermal_zone*; do
        type=$(cat "$zone/type" 2>/dev/null)
        temp=$(cat "$zone/temp" 2>/dev/null)
        case "$type" in cpu*) cpu_temp=${temp:-0} ;; npu*) npu_temp=${temp:-0} ;; esac
      done

      restarts=$(cat /var/run/rv1126-object-watchdog.state 2>/dev/null)
      [ -n "$restarts" ] || restarts=0
      errcnt=$(sed -n "s/^Interrupt .*ErrCnt:\([0-9][0-9]*\).*/\1/p" /proc/rkispp0 2>/dev/null | head -1)
      [ -n "$errcnt" ] || errcnt=-1
      line=$(grep "^video " /tmp/rv1126-object.log 2>/dev/null | tail -1)
      main=$(echo "$line" | sed -n "s/^video main(venc=\([0-9][0-9]*\).*/\1/p")
      sub=$(echo "$line" | sed -n "s/^video .* sub(venc=\([0-9][0-9]*\).*/\1/p")
      ai=$(echo "$line" | sed -n "s/.*completed=\([0-9][0-9]*\).*/\1/p")
      osd=$(echo "$line" | sed -n "s/.*osd(boxes=\([0-9][0-9]*\).*/\1/p")
      audio=$(echo "$line" | sed -n "s/.*audio(captured=[0-9][0-9]*, sent=\([0-9][0-9]*\).*/\1/p")

      echo "$pid,$cpu,${rss:-0},${hwm:-0},${threads:-0},$fds,$cpu_temp,$npu_temp,$restarts,$errcnt,${main:-0},${sub:-0},${ai:-0},${osd:-0},${audio:-0}"
    ' 2>>"$output_dir/telemetry-errors.log" | tr -d '\r')
    printf '%s,%s,%s\n' "$host_time" "$host_network_reachable" "$row" \
      >>"$output_dir/resources.csv"

    for _ in $(seq 1 55); do
      [[ -e "$stop_file" || $(date +%s) -ge $end_epoch ]] && break
      sleep 1
    done
  done
}

telemetry_loop &
telemetry_pid=$!

rtsp_pass=0
rtsp_fail=0
health_pass=0
health_fail=0
av_pass=0
av_fail=0
round=0
next_av=$start_epoch

while (( $(date +%s) < end_epoch )); do
  now=$(date +%s)
  remaining=$((end_epoch - now))
  (( remaining >= 60 )) || break
  duration=$cycle_seconds
  (( duration <= remaining )) || duration=$remaining
  round=$((round + 1))
  round_stamp=$(date +%Y%m%d-%H%M%S)
  round_log="$output_dir/rtsp/round-$(printf '%03d' "$round")-$round_stamp.log"

  echo "$(date -Is) round=$round duration=${duration}s START" | tee -a "$output_dir/events.log"
  if bash "$project_dir/scripts/test_rtsp_tcp.sh" "$board_ip" "$duration" 30 \
      >"$round_log" 2>&1; then
    rtsp_pass=$((rtsp_pass + 1))
    tail -4 "$round_log" | tee -a "$output_dir/events.log"
  else
    rtsp_fail=$((rtsp_fail + 1))
    echo "$(date -Is) round=$round RTSP_FAIL" | tee -a "$output_dir/events.log"
    tail -80 "$round_log" >>"$output_dir/events.log"
    cp /tmp/rv1126_tcp_test/main.log "$output_dir/failures/round-${round}-main.log" 2>/dev/null || true
    cp /tmp/rv1126_tcp_test/sub.log "$output_dir/failures/round-${round}-sub.log" 2>/dev/null || true
  fi

  health=$(adb shell '/root/rv1126_objects/rv1126-object health 2>&1 || true' | tr -d '\r')
  printf '%s %s\n' "$(date -Is)" "$health" >>"$output_dir/health.log"
  if [[ "$health" == Healthy:* ]]; then
    health_pass=$((health_pass + 1))
  else
    health_fail=$((health_fail + 1))
  fi
  adb shell '/root/rv1126_objects/rv1126-object watchdog-status 2>&1 || true' \
    | tr -d '\r' | sed "s/^/$(date -Is) /" >>"$output_dir/watchdog.log"

  now=$(date +%s)
  if (( now >= next_av && now + 60 < end_epoch )); then
    av_log="$output_dir/av/av-$(date +%Y%m%d-%H%M%S).log"
    if bash "$project_dir/scripts/test_rtsp_av.sh" "$board_ip" 30 >"$av_log" 2>&1; then
      av_pass=$((av_pass + 1))
    else
      av_fail=$((av_fail + 1))
      tail -80 "$av_log" >>"$output_dir/events.log"
    fi
    next_av=$((next_av + 3600))
  fi
done

touch "$stop_file"
wait "$telemetry_pid" 2>/dev/null || true

end_iso=$(date -Is)
adb shell '/root/rv1126_objects/rv1126-object status 2>&1 || true; /root/rv1126_objects/rv1126-object health 2>&1 || true; /root/rv1126_objects/rv1126-object watchdog-status 2>&1 || true' \
  >"$output_dir/final-health.txt" 2>&1 || true
adb shell 'cat /proc/rkispp0 2>/dev/null' >"$output_dir/rkispp-after.txt" 2>&1 || true
adb shell 'tail -5000 /tmp/rv1126-object.log 2>/dev/null' >"$output_dir/application-tail.log" 2>&1 || true
adb shell 'cat /tmp/rv1126-object-watchdog.log 2>/dev/null' >"$output_dir/watchdog-full.log" 2>&1 || true
adb shell 'dmesg | tail -2000' >"$output_dir/dmesg-after.txt" 2>&1 || true

python3 - "$output_dir/resources.csv" "$output_dir/metrics-summary.txt" <<'PY'
import csv
import math
import sys

source, destination = sys.argv[1:]
with open(source, newline="", encoding="utf-8") as handle:
    rows = list(csv.DictReader(handle))

def values(name):
    result = []
    for row in rows:
        try:
            result.append(float(row[name]))
        except (KeyError, TypeError, ValueError):
            pass
    return result

def percentile(data, value):
    if not data:
        return float("nan")
    ordered = sorted(data)
    rank = max(1, math.ceil(len(ordered) * value / 100))
    return ordered[rank - 1]

cpu = values("cpu_total_percent")
rss = values("rss_kb")
threads = values("threads")
fds = values("fd_count")
cpu_temp = values("cpu_temp_mc")
npu_temp = values("npu_temp_mc")
restarts = values("restarts")
errors = values("errcnt")
osd = values("osd_boxes")
network = values("host_network_reachable")

with open(destination, "w", encoding="utf-8") as output:
    output.write(f"samples={len(rows)}\n")
    output.write(f"cpu_p50={percentile(cpu, 50):.2f}\n")
    output.write(f"cpu_p95={percentile(cpu, 95):.2f}\n")
    output.write(f"cpu_p99={percentile(cpu, 99):.2f}\n")
    output.write(f"cpu_max={max(cpu, default=float('nan')):.2f}\n")
    output.write(f"rss_start_kb={rss[0] if rss else float('nan'):.0f}\n")
    output.write(f"rss_end_kb={rss[-1] if rss else float('nan'):.0f}\n")
    output.write(f"rss_max_kb={max(rss, default=float('nan')):.0f}\n")
    output.write(f"threads_min={min(threads, default=float('nan')):.0f}\n")
    output.write(f"threads_max={max(threads, default=float('nan')):.0f}\n")
    output.write(f"fd_min={min(fds, default=float('nan')):.0f}\n")
    output.write(f"fd_max={max(fds, default=float('nan')):.0f}\n")
    output.write(f"cpu_temp_max_mc={max(cpu_temp, default=float('nan')):.0f}\n")
    output.write(f"npu_temp_max_mc={max(npu_temp, default=float('nan')):.0f}\n")
    output.write(f"watchdog_restarts_max={max(restarts, default=float('nan')):.0f}\n")
    output.write(f"errcnt_max={max(errors, default=float('nan')):.0f}\n")
    output.write(f"osd_boxes_max={max(osd, default=float('nan')):.0f}\n")
    output.write(f"host_network_unreachable_samples={sum(value < 0.5 for value in network)}\n")
PY

{
  echo "# RV1126 long-duration acceptance report"
  echo
  echo "- Start: $start_iso"
  echo "- End: $end_iso"
  echo "- Board: $board_ip"
  echo "- Requested duration: $total_seconds seconds"
  echo "- Git commit: $git_commit"
  echo "- RTSP cycles: pass=$rtsp_pass fail=$rtsp_fail"
  echo "- Health checks: pass=$health_pass fail=$health_fail"
  echo "- Audio/video checks: pass=$av_pass fail=$av_fail"
  echo
  echo '```text'
  cat "$output_dir/metrics-summary.txt"
  echo '```'
  echo
  echo "Final health:"
  echo '```text'
  cat "$output_dir/final-health.txt"
  echo '```'
} >"$output_dir/report.md"

overall=PASS
(( rtsp_fail == 0 && health_fail == 0 && av_fail == 0 )) || overall=FAIL
grep -q '^Healthy:' "$output_dir/final-health.txt" || overall=FAIL
network_failures=$(sed -n 's/^host_network_unreachable_samples=//p' \
  "$output_dir/metrics-summary.txt")
if [[ "${network_failures:-0}" -gt 0 ]]; then
  overall=INVALID_NETWORK
fi
echo "result=$overall" | tee -a "$output_dir/result.txt"
echo "output_dir=$output_dir" | tee -a "$output_dir/result.txt"
echo "report=$output_dir/report.md" | tee -a "$output_dir/result.txt"
[[ "$overall" == PASS ]]
