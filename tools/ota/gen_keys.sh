#!/usr/bin/env bash
# gen_keys.sh — one-time OTA signing key generation.
#
# Creates an RSA-2048 private key (kept here, gitignored) and prints the public
# key as a C string to paste into src/ota_signing.h (kOtaPublicKeyPem).
#
# Keep ota_private_key.pem OFFLINE and secret. Never commit it.
set -euo pipefail
cd "$(dirname "$0")"

KEY="ota_private_key.pem"
PUB="ota_public_key.pem"

if [[ -f "$KEY" ]]; then
  echo "refusing to overwrite existing $KEY (delete it first to regenerate)" >&2
  exit 1
fi

# Create the key owner-only from the start (no world-readable window before a
# later chmod); the umask is scoped to this subshell.
(umask 077 && openssl genrsa -out "$KEY" 2048)
chmod 600 "$KEY"
openssl rsa -in "$KEY" -pubout -out "$PUB"

echo
echo "Private key: $(pwd)/$KEY   (keep offline, do not commit)"
echo "Public key:  $(pwd)/$PUB"
echo
echo "Paste this into firmware/src/ota_signing.h as kOtaPublicKeyPem:"
echo "----------------------------------------------------------------"
awk '{ printf "    \"%s\\n\"\n", $0 }' "$PUB"
echo "----------------------------------------------------------------"
