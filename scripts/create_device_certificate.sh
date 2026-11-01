#!/usr/bin/env bash
set -euo pipefail

if (( $# < 2 || $# > 3 )); then
  echo "Usage: $0 OUTSIDE_REPOSITORY_DIRECTORY DEVICE_NAME [DEVICE_IP]" >&2
  exit 2
fi

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
mkdir -p "$1"
output_dir="$(cd -- "$1" && pwd -P)"
case "$output_dir/" in
  "$project_dir"/*)
    echo "Refusing to store a TLS private key inside the repository" >&2
    exit 1
    ;;
esac

device_name="$2"
device_ip="${3:-}"
if [[ ! "$device_name" =~ ^[A-Za-z0-9._-]{1,64}$ ]]; then
  echo "Invalid device name" >&2
  exit 2
fi
key="$output_dir/${device_name}.key"
cert="$output_dir/${device_name}.crt"
[[ ! -e "$key" && ! -e "$cert" ]] || {
  echo "Refusing to overwrite an existing device certificate" >&2
  exit 1
}

san="DNS:${device_name}"
[[ -z "$device_ip" ]] || san+=",IP:${device_ip}"
umask 077
openssl req -x509 -newkey rsa:3072 -sha256 -nodes -days 825 \
  -subj "/CN=${device_name}/O=RV1126 Vision" \
  -addext "subjectAltName=${san}" \
  -keyout "$key" -out "$cert"
chmod 0600 "$key"
chmod 0644 "$cert"
echo "Device TLS key: $key"
echo "Device certificate: $cert"
echo "For production, replace this self-signed certificate with one signed by your device CA."
