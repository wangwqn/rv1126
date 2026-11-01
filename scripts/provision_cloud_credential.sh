#!/usr/bin/env bash
set -euo pipefail

if (( $# != 1 )); then
  echo "Usage: $0 DEVICE_CREDENTIAL_ENV" >&2
  exit 2
fi

credential="$(realpath "$1")"
[[ -s "$credential" ]] || { echo "Credential file is missing" >&2; exit 1; }
grep -q '^DEVICE_ID=' "$credential" || { echo "DEVICE_ID is missing" >&2; exit 1; }
grep -q '^API_BASE_URL=https://' "$credential" || { echo "HTTPS API_BASE_URL is missing" >&2; exit 1; }
grep -q '^DEVICE_HMAC_SECRET=.' "$credential" || { echo "DEVICE_HMAC_SECRET is missing" >&2; exit 1; }
[[ "$(adb get-state 2>/dev/null || true)" == device ]] || { echo "No online ADB device" >&2; exit 1; }

adb shell 'mkdir -p /userdata/rv1126-security /root/rv1126_cloud && chmod 0700 /userdata/rv1126-security /root/rv1126_cloud'
adb push "$credential" /userdata/rv1126-security/device-credential.env.upload
adb shell '
set -e
chmod 0600 /userdata/rv1126-security/device-credential.env.upload
mv -f /userdata/rv1126-security/device-credential.env.upload /userdata/rv1126-security/device-credential.env
ln -sfn /userdata/rv1126-security/device-credential.env /root/rv1126_cloud/device.env
sync
'
echo "Cloud device credential provisioned separately from the Web management token."
