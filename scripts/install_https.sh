#!/usr/bin/env bash
set -euo pipefail

if (( $# != 2 )); then
  echo "Usage: $0 DEVICE_CERTIFICATE DEVICE_PRIVATE_KEY" >&2
  exit 2
fi

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
certificate="$(realpath "$1")"
private_key="$(realpath "$2")"
[[ -s "$certificate" && -s "$private_key" ]] || {
  echo "Certificate or key is missing" >&2
  exit 1
}
[[ "$(adb get-state 2>/dev/null || true)" == device ]] || {
  echo "No online ADB device" >&2
  exit 1
}

adb shell 'mkdir -p /userdata/rv1126-security/tls && chmod 0700 /userdata/rv1126-security /userdata/rv1126-security/tls'
adb push "$certificate" /userdata/rv1126-security/tls/device.crt
adb push "$private_key" /userdata/rv1126-security/tls/device.key
adb push "$project_dir/deploy/security/nginx-rv1126.conf" /tmp/nginx-rv1126.conf
adb shell '
set -e
chmod 0644 /userdata/rv1126-security/tls/device.crt
chmod 0600 /userdata/rv1126-security/tls/device.key
if [ ! -e /etc/nginx/nginx.conf.factory ]; then cp -p /etc/nginx/nginx.conf /etc/nginx/nginx.conf.factory; fi
nginx -t -c /tmp/nginx-rv1126.conf
/etc/init.d/S50nginx stop 2>/dev/null || true
cp /tmp/nginx-rv1126.conf /etc/nginx/nginx.conf
rm -f /tmp/nginx-rv1126.conf
/etc/init.d/S50nginx start
/root/rv1126_objects/rv1126-web restart
netstat -lnt | grep -E "127.0.0.1:8080|:443[[:space:]]"
'
echo "HTTPS installed. Open https://BOARD_IP/ and trust your device CA certificate."
