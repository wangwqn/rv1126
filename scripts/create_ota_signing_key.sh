#!/usr/bin/env bash
set -euo pipefail

if (( $# != 1 )); then
  echo "Usage: $0 OUTSIDE_REPOSITORY_KEY_DIRECTORY" >&2
  exit 2
fi

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
mkdir -p "$1"
key_dir="$(cd -- "$1" && pwd -P)"
case "$key_dir/" in
  "$project_dir"/*)
    echo "Refusing to store an OTA private key inside the repository" >&2
    exit 1
    ;;
esac

private_key="$key_dir/rv1126-ota-signing-private.pem"
public_pem="$key_dir/rv1126-ota-signing-public.pem"
if [[ -e "$private_key" || -e "$public_pem" ]]; then
  echo "Refusing to overwrite an existing OTA key" >&2
  exit 1
fi

umask 077
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:3072 -out "$private_key"
openssl pkey -in "$private_key" -pubout -out "$public_pem"
modulus="$(openssl rsa -in "$private_key" -noout -modulus | sed 's/^Modulus=//' | tr 'A-F' 'a-f')"
key_id="$(openssl pkey -in "$private_key" -pubout -outform DER | sha256sum | cut -c1-16)"
trusted_json="$key_dir/${key_id}.json"

python3 - "$trusted_json" "$key_id" "$modulus" <<'PY'
import json
import os
import sys

path, key_id, modulus = sys.argv[1:]
value = {
    "schema": 1,
    "algorithm": "rsa-pkcs1v15-sha256",
    "key_id": key_id,
    "exponent": 65537,
    "modulus_hex": modulus,
}
temporary = path + ".new"
with open(temporary, "w", encoding="utf-8") as handle:
    json.dump(value, handle, sort_keys=True, indent=2)
    handle.write("\n")
    handle.flush()
    os.fsync(handle.fileno())
os.chmod(temporary, 0o644)
os.replace(temporary, path)
PY

chmod 0600 "$private_key"
chmod 0644 "$public_pem" "$trusted_json"
echo "Private signing key (keep offline): $private_key"
echo "Trusted device public key: $trusted_json"
echo "Key ID: $key_id"
