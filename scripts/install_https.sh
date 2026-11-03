#!/usr/bin/env bash
set -euo pipefail

if (( $# < 2 || $# > 3 )); then
  echo "Usage: $0 DEVICE_CERTIFICATE DEVICE_PRIVATE_KEY [CA_CHAIN]" >&2
  exit 2
fi

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
certificate="$(realpath "$1")"
private_key="$(realpath "$2")"
ca_chain="${3:-}"
[[ -z "$ca_chain" ]] || ca_chain="$(realpath "$ca_chain")"
[[ -s "$certificate" && -s "$private_key" ]] || {
  echo "Certificate or key is missing" >&2
  exit 1
}
openssl x509 -in "$certificate" -noout -checkend 86400 >/dev/null || {
  echo "Certificate is expired or expires within 24 hours" >&2; exit 1;
}
certificate_public=$(openssl x509 -in "$certificate" -pubkey -noout | openssl pkey -pubin -outform DER | sha256sum | awk '{print $1}')
key_public=$(openssl pkey -in "$private_key" -pubout -outform DER | sha256sum | awk '{print $1}')
[[ "$certificate_public" == "$key_public" ]] || { echo "Certificate and private key do not match" >&2; exit 1; }
if [[ -n "$ca_chain" ]]; then
  [[ -s "$ca_chain" ]] || { echo "CA chain is missing" >&2; exit 1; }
  openssl verify -CAfile "$ca_chain" "$certificate"
fi
[[ "$(adb get-state 2>/dev/null || true)" == device ]] || {
  echo "No online ADB device" >&2
  exit 1
}

adb shell 'mkdir -p /userdata/rv1126-security/tls && chmod 0700 /userdata/rv1126-security /userdata/rv1126-security/tls'
fullchain="$(mktemp /tmp/rv1126-fullchain.XXXXXX)"
trap 'rm -f "$fullchain"' EXIT
cp "$certificate" "$fullchain"
[[ -z "$ca_chain" ]] || { printf '\n' >>"$fullchain"; sed -n '/-----BEGIN CERTIFICATE-----/,/-----END CERTIFICATE-----/p' "$ca_chain" >>"$fullchain"; }
adb push "$fullchain" /userdata/rv1126-security/tls/device.crt.new
adb push "$private_key" /userdata/rv1126-security/tls/device.key.new
adb push "$project_dir/deploy/security/nginx-rv1126.conf" /tmp/nginx-rv1126.conf
adb shell '
set -e
chmod 0644 /userdata/rv1126-security/tls/device.crt.new
chmod 0600 /userdata/rv1126-security/tls/device.key.new
if [ ! -e /etc/nginx/nginx.conf.factory ]; then cp -p /etc/nginx/nginx.conf /etc/nginx/nginx.conf.factory; fi
cp -p /userdata/rv1126-security/tls/device.crt /userdata/rv1126-security/tls/device.crt.previous 2>/dev/null || true
cp -p /userdata/rv1126-security/tls/device.key /userdata/rv1126-security/tls/device.key.previous 2>/dev/null || true
mv /userdata/rv1126-security/tls/device.crt.new /userdata/rv1126-security/tls/device.crt
mv /userdata/rv1126-security/tls/device.key.new /userdata/rv1126-security/tls/device.key
cp /tmp/nginx-rv1126.conf /etc/nginx/nginx.conf
rm -f /tmp/nginx-rv1126.conf
if ! nginx -t; then
  cp /userdata/rv1126-security/tls/device.crt.previous /userdata/rv1126-security/tls/device.crt 2>/dev/null || true
  cp /userdata/rv1126-security/tls/device.key.previous /userdata/rv1126-security/tls/device.key 2>/dev/null || true
  exit 1
fi
if [ -s /var/run/nginx.pid ]; then nginx -s reload; else /etc/init.d/S50nginx start; fi
/root/rv1126_objects/rv1126-web restart
netstat -lnt | grep -E "127.0.0.1:8080|:443[[:space:]]"
'
echo "HTTPS certificate installed atomically. Import the CA certificate into the client trust store before browsing."
