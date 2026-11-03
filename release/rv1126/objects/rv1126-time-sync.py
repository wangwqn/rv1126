#!/usr/bin/env python3
"""Small dependency-free SNTP client for the RV1126 Buildroot image."""

import argparse
import json
import os
import socket
import statistics
import struct
import subprocess
import tempfile
import time
from pathlib import Path


NTP_EPOCH = 2208988800
MIN_UNIX_TIME = 1704067200  # 2024-01-01, rejects an uninitialised RTC.
MAX_UNIX_TIME = 4133980800  # 2101-01-01.
DEFAULT_SERVERS = (
    "ntp.aliyun.com",
    "time1.cloud.tencent.com",
    "cn.pool.ntp.org",
    "time.cloudflare.com",
)


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=str(path.parent))
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            json.dump(value, handle, ensure_ascii=False, sort_keys=True)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def query(server, timeout=3.0):
    request = bytearray(48)
    request[0] = 0x23  # LI=0, version=4, client mode.
    started = time.monotonic()
    addresses = socket.getaddrinfo(server, 123, socket.AF_UNSPEC, socket.SOCK_DGRAM)
    last_error = None
    for family, socktype, protocol, _, address in addresses:
        try:
            with socket.socket(family, socktype, protocol) as client:
                client.settimeout(timeout)
                client.sendto(request, address)
                response, _ = client.recvfrom(512)
            if len(response) < 48:
                raise ValueError("short NTP response")
            leap = response[0] >> 6
            mode = response[0] & 7
            stratum = response[1]
            if leap == 3 or mode not in (4, 5) or not 1 <= stratum <= 15:
                raise ValueError("invalid NTP server state")
            seconds, fraction = struct.unpack("!II", response[40:48])
            unix_time = seconds - NTP_EPOCH + fraction / float(1 << 32)
            if not MIN_UNIX_TIME <= unix_time < MAX_UNIX_TIME:
                raise ValueError("NTP time outside accepted range")
            return unix_time, round((time.monotonic() - started) * 1000, 2), stratum
        except (OSError, ValueError) as error:
            last_error = error
    raise RuntimeError("{}: {}".format(server, last_error or "no address"))


def sample(servers, timeout):
    values, errors = [], {}
    for server in servers:
        try:
            timestamp, latency_ms, stratum = query(server, timeout)
            values.append((timestamp, server, latency_ms, stratum))
        except Exception as error:  # every server is an independent fallback
            errors[server] = str(error)[:160]
    if not values:
        raise RuntimeError("all NTP servers failed: " + json.dumps(errors, ensure_ascii=False))
    timestamps = [item[0] for item in values]
    median = statistics.median(timestamps)
    close = [item for item in values if abs(item[0] - median) <= 5.0]
    # A valid RTC permits one reachable server.  An invalid RTC accepts one
    # server only as a bootstrap fallback; later periodic samples correct it.
    if len(values) >= 2 and len(close) < 2:
        raise RuntimeError("NTP servers disagree by more than five seconds")
    chosen = statistics.median(item[0] for item in (close or values))
    return int(round(chosen)), values, errors


def set_clock(timestamp):
    subprocess.run(["/bin/date", "-u", "-s", "@{}".format(timestamp)], check=True,
                   stdout=subprocess.DEVNULL)
    rtc = subprocess.run(["/sbin/hwclock", "-w"], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True)
    return rtc.returncode == 0, rtc.stdout.strip()[:160]


def sync_once(args):
    attempted_at = int(time.time())
    timestamp, samples, errors = sample(args.servers, args.timeout)
    rtc_ok, rtc_message = set_clock(timestamp)
    result = {
        "ok": True,
        "synced_at": timestamp,
        "attempted_at": attempted_at,
        "sources": [
            {"server": server, "unix_time": round(value, 3),
             "latency_ms": latency, "stratum": stratum}
            for value, server, latency, stratum in samples
        ],
        "errors": errors,
        "rtc_written": rtc_ok,
    }
    if rtc_message:
        result["rtc_message"] = rtc_message
    atomic_json(args.status, result)
    return result


def main():
    parser = argparse.ArgumentParser(description="RV1126 dependency-free SNTP synchroniser")
    parser.add_argument("--daemon", action="store_true")
    parser.add_argument("--server", action="append", dest="servers")
    parser.add_argument("--timeout", type=float, default=3.0)
    parser.add_argument("--retry-seconds", type=int, default=15)
    parser.add_argument("--interval-seconds", type=int, default=21600)
    parser.add_argument("--status", default="/userdata/rv1126-time/status.json")
    args = parser.parse_args()
    args.servers = tuple(args.servers or DEFAULT_SERVERS)
    if not 0.5 <= args.timeout <= 10 or not 5 <= args.retry_seconds <= 3600 or not 300 <= args.interval_seconds <= 604800:
        parser.error("invalid timeout or interval")
    while True:
        try:
            result = sync_once(args)
            print(json.dumps(result, ensure_ascii=False, sort_keys=True), flush=True)
            delay = args.interval_seconds
        except Exception as error:
            atomic_json(args.status, {"ok": False, "attempted_at": int(time.time()),
                                      "error": str(error)[:512]})
            print("time sync failed: {}".format(error), flush=True)
            if not args.daemon:
                raise SystemExit(1)
            delay = args.retry_seconds
        if not args.daemon:
            return
        time.sleep(delay)


if __name__ == "__main__":
    main()
