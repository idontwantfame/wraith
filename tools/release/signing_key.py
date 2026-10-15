#!/usr/bin/env python3
"""The OTA public key the firmware trusts, read from src/ota_signing.h.

    python3 tools/release/signing_key.py public-key            # print it (PEM)
    python3 tools/release/signing_key.py check-firmware FILE   # fail unless FILE embeds it

The release workflow runs both against the checked-out source, not the build
artifacts, before it signs anything: a build that swapped in another trusted
key is refused.
"""

import re
import sys
from pathlib import Path

SIGNING_HEADER = Path("src/ota_signing.h")


def public_key_pem(header: Path = SIGNING_HEADER) -> str:
    source = header.read_text()
    match = re.search(r"kOtaPublicKeyPem\[\]\s*=(.*?);", source, re.S)
    if not match:
        raise ValueError(f"kOtaPublicKeyPem not found in {header}")
    pem = "".join(re.findall(r'"([^"]*)"', match.group(1))).replace("\\n", "\n")
    if "BEGIN PUBLIC KEY" not in pem or "PRIVATE" in pem:
        raise ValueError(f"kOtaPublicKeyPem in {header} is not a public key")
    return pem


def firmware_embeds_key(firmware: Path, pem: str) -> bool:
    """The firmware carries the key as the PEM text of kOtaPublicKeyPem."""
    return pem.encode() in firmware.read_bytes()


def main(argv: list[str]) -> int:
    try:
        pem = public_key_pem()
    except (OSError, ValueError) as error:
        print(f"signing_key.py: {error}", file=sys.stderr)
        return 1
    if argv[1:] == ["public-key"]:
        sys.stdout.write(pem)
        return 0
    if len(argv) == 3 and argv[1] == "check-firmware":
        firmware = Path(argv[2])
        if firmware_embeds_key(firmware, pem):
            print(f"{firmware} embeds the public key from {SIGNING_HEADER}")
            return 0
        print(f"signing_key.py: {firmware} does not embed the public key from {SIGNING_HEADER}",
              file=sys.stderr)
        return 1
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
