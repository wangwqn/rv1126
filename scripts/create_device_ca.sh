#!/usr/bin/env bash
set -euo pipefail

if (( $# != 2 )); then
  echo "Usage: $0 OUTSIDE_REPOSITORY_DIRECTORY CA_COMMON_NAME" >&2
  exit 2
fi
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
mkdir -p "$1"
output_dir="$(cd -- "$1" && pwd -P)"
case "$output_dir/" in "$project_dir"/*) echo "CA secrets must stay outside the repository" >&2; exit 1;; esac
name="$2"
[[ "$name" =~ ^[A-Za-z0-9._[:space:]-]{1,64}$ ]] || { echo "Invalid CA name" >&2; exit 2; }
key="$output_dir/rv1126-device-ca.key"
certificate="$output_dir/rv1126-device-ca.crt"
[[ ! -e "$key" && ! -e "$certificate" ]] || { echo "Refusing to overwrite an existing CA" >&2; exit 1; }
umask 077
csr="$(mktemp /tmp/rv1126-device-ca.XXXXXX.csr)"
extension="$(mktemp /tmp/rv1126-device-ca.XXXXXX.ext)"
trap 'rm -f "$csr" "$extension"' EXIT
printf '%s\n' \
  '[ca]' \
  'basicConstraints=critical,CA:TRUE,pathlen:0' \
  'keyUsage=critical,keyCertSign,cRLSign' \
  'subjectKeyIdentifier=hash' \
  'authorityKeyIdentifier=keyid:always' >"$extension"
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:3072 -out "$key"
openssl req -new -sha256 -key "$key" -out "$csr" \
  -subj "/CN=${name}/O=RV1126 Vision Device CA"
openssl x509 -req -sha256 -days 3650 -in "$csr" -signkey "$key" \
  -extfile "$extension" -extensions ca -out "$certificate"
openssl verify -CAfile "$certificate" "$certificate"
chmod 0600 "$key"
chmod 0644 "$certificate"
echo "Offline CA private key: $key"
echo "Browser trust certificate: $certificate"
echo "Keep the private key offline; import only the .crt into browser/OS trust stores."
