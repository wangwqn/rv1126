#!/usr/bin/env bash
set -euo pipefail

if (( $# < 5 )); then
  echo "Usage: $0 CA_CERT CA_KEY OUTSIDE_OUTPUT_DIR DEVICE_NAME IP_OR_DNS [IP_OR_DNS ...]" >&2
  exit 2
fi
ca_cert="$(realpath "$1")"
ca_key="$(realpath "$2")"
mkdir -p "$3"
output_dir="$(cd -- "$3" && pwd -P)"
device_name="$4"
shift 4
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
case "$output_dir/" in "$project_dir"/*) echo "Device private keys must stay outside the repository" >&2; exit 1;; esac
[[ -s "$ca_cert" && -s "$ca_key" ]] || { echo "CA certificate or key is missing" >&2; exit 1; }
[[ "$device_name" =~ ^[A-Za-z0-9._-]{1,64}$ ]] || { echo "Invalid device name" >&2; exit 2; }
key="$output_dir/${device_name}.key"
csr="$output_dir/${device_name}.csr"
certificate="$output_dir/${device_name}.crt"
[[ ! -e "$key" && ! -e "$certificate" ]] || { echo "Refusing to overwrite an existing device certificate" >&2; exit 1; }

san="DNS:${device_name}"
for value in "$@"; do
  if [[ "$value" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then san+=",IP:${value}";
  elif [[ "$value" =~ ^[A-Za-z0-9._-]{1,253}$ ]]; then san+=",DNS:${value}";
  else echo "Invalid SAN: $value" >&2; exit 2; fi
done
extension="$(mktemp /tmp/rv1126-device-cert.XXXXXX)"
trap 'rm -f "$extension" "$csr"' EXIT
printf '%s\n' \
  '[server]' \
  'basicConstraints=critical,CA:FALSE' \
  'keyUsage=critical,digitalSignature,keyEncipherment' \
  'extendedKeyUsage=serverAuth' \
  "subjectAltName=${san}" \
  'subjectKeyIdentifier=hash' \
  'authorityKeyIdentifier=keyid,issuer' >"$extension"
umask 077
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:3072 -out "$key"
openssl req -new -sha256 -key "$key" -out "$csr" -subj "/CN=${device_name}/O=RV1126 Vision"
openssl x509 -req -sha256 -days 397 -in "$csr" -CA "$ca_cert" -CAkey "$ca_key" \
  -CAcreateserial -extfile "$extension" -extensions server -out "$certificate"
openssl verify -CAfile "$ca_cert" "$certificate"
chmod 0600 "$key"
chmod 0644 "$certificate"
echo "Device key: $key"
echo "CA-signed certificate: $certificate"
echo "SAN: $san"
