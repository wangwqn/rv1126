#!/usr/bin/env bash
set -euo pipefail

if (( $# != 1 )); then
  echo "Usage: $0 TRUSTED_PUBLIC_KEY_JSON" >&2
  exit 2
fi

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
trusted_key="$(realpath "$1")"
[[ -s "$trusted_key" ]] || { echo "Trusted key is missing" >&2; exit 1; }
key_id="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["key_id"])' "$trusted_key")"
[[ "$key_id" =~ ^[A-Za-z0-9._-]{8,64}$ ]] || { echo "Invalid key ID" >&2; exit 1; }
[[ "$(adb get-state 2>/dev/null || true)" == device ]] || { echo "No online ADB device" >&2; exit 1; }

adb shell "mkdir -p /root/rv1126_ota /userdata/rv1126-ota/trusted_keys"
adb push "$project_dir/deploy/ota/rv1126_ota.py" /root/rv1126_ota/rv1126_ota.py
adb push "$project_dir/deploy/ota/rv1126-ota-recover" /root/rv1126_ota/rv1126-ota-recover
adb push "$trusted_key" "/userdata/rv1126-ota/trusted_keys/${key_id}.json"
adb shell "
set -e
chmod 0755 /root/rv1126_ota/rv1126_ota.py /root/rv1126_ota/rv1126-ota-recover
chmod 0644 '/userdata/rv1126-ota/trusted_keys/${key_id}.json'
ln -sfn /root/rv1126_ota/rv1126-ota-recover /etc/init.d/S97rv1126-ota-recover
/usr/bin/python3 /root/rv1126_ota/rv1126_ota.py bootstrap
"
echo "OTA runtime installed with trusted key $key_id"
