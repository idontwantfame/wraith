#!/usr/bin/env bash
# sign.sh — produce a detached base64 signature for an OTA image.
#
#   tools/ota/sign.sh .pio/build/esp32c3/firmware.bin
#   -> firmware.bin.sig   (base64, RSA-2048 / SHA-256, PKCS#1 v1.5)
#
# Upload firmware.bin + firmware.bin.sig together in the web UI, or via curl. The
# signature is base64, so URL-encode it ('+' would otherwise decode as a space),
# and send the X-Requested-With header the device requires on every mutation:
#   SIG=$(python3 -c 'import sys,urllib.parse;print(urllib.parse.quote(open(sys.argv[1]).read().strip(),safe=""))' firmware.bin.sig)
#   curl -H "X-Requested-With: fogger" -F firmware=@firmware.bin "http://<ip>/api/ota?sig=$SIG"
set -euo pipefail

KEY="$(dirname "$0")/ota_private_key.pem"
IMG="${1:-}"

if [[ -z "$IMG" || ! -f "$IMG" ]]; then
  echo "usage: $0 <firmware.bin>" >&2
  exit 1
fi
if [[ ! -f "$KEY" ]]; then
  echo "missing $KEY — run gen_keys.sh first" >&2
  exit 1
fi

SIG="${IMG}.sig"
openssl dgst -sha256 -sign "$KEY" "$IMG" | openssl base64 -A > "$SIG"
echo "wrote $SIG"
