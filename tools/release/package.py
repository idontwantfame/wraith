#!/usr/bin/env python3
"""Collect a built firmware into release assets (no secrets involved).

Run from the repo root after `pio run`:

    python3 tools/release/package.py v1.2.0            # -> release/

Writes release/assets/:
  wraith-<version>-firmware.bin  app image (web UI included), for updates over
                                 Wi-Fi (gets a .sig)
  wraith-<version>-factory.bin   complete image for a first install at offset 0
                                 (bootloader, partition table, app)
  ota_public_key.pem             the key the firmware trusts, to verify .sig files
and release/notes.md (build number, files, install commands). Signing happens
later, in the release job that holds the private key.
"""

import argparse
import os
import re
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from signing_key import public_key_pem  # noqa: E402 (sibling module)

BUILD_DIR = Path(".pio/build/esp32c3")
MARKER_MAGIC = b"WRAITHFW"  # fw_marker.h: magic + uint32 build, little-endian
VERSION_PATTERN = re.compile(r"^[0-9A-Za-z][0-9A-Za-z._-]{0,63}$")


def fail(message: str) -> None:
    sys.exit(f"package.py: {message}")


def read_build(firmware: bytes) -> int:
    if firmware.count(MARKER_MAGIC) != 1:
        fail("firmware.bin must contain exactly one build marker")
    start = firmware.find(MARKER_MAGIC) + len(MARKER_MAGIC)
    return int.from_bytes(firmware[start:start + 4], "little")


def notes(version: str, build: int) -> str:
    name = f"wraith-{version}"
    return f"""Build number **{build}** (the UTC build time, `yymmddHHMM`). The web UI is
built into the firmware, so one image updates both.

| File | Use |
|---|---|
| `{name}-firmware.bin` + `.sig` | Update over Wi-Fi: Settings → Firmware update (pick both files) |
| `{name}-factory.bin` | First install over USB, at offset `0x0`. **Erases saved settings.** |
| `ota_public_key.pem` | The key the firmware trusts; verifies the `.sig` files |
| `SHA256SUMS` + `.sig` | Checksums of every file, signed with the same key |

First install (esptool 5; with esptool 4 use `write_flash`):

```
esptool --chip esp32c3 write-flash 0x0 {name}-factory.bin
```

Verify a download:

```
openssl base64 -d -A -in SHA256SUMS.sig -out SHA256SUMS.sig.raw
openssl dgst -sha256 -verify ota_public_key.pem -signature SHA256SUMS.sig.raw SHA256SUMS
sha256sum -c SHA256SUMS
```
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("version", help="release version, e.g. v1.2.0")
    parser.add_argument("--out", type=Path, default=Path("release"))
    args = parser.parse_args()
    if not VERSION_PATTERN.fullmatch(args.version):
        fail(f"'{args.version}' is not a usable version (letters, digits, . _ -)")

    inputs = {name: BUILD_DIR / name for name in ("firmware.bin", "firmware.factory.bin")}
    missing = [str(p) for p in inputs.values() if not p.is_file()]
    if missing:
        fail(f"missing {', '.join(missing)}; run `pio run` first")

    build = read_build(inputs["firmware.bin"].read_bytes())
    name = f"wraith-{args.version}"

    assets = args.out / "assets"
    shutil.rmtree(args.out, ignore_errors=True)
    assets.mkdir(parents=True)
    shutil.copyfile(inputs["firmware.bin"], assets / f"{name}-firmware.bin")
    shutil.copyfile(inputs["firmware.factory.bin"], assets / f"{name}-factory.bin")
    try:
        (assets / "ota_public_key.pem").write_text(public_key_pem())
    except ValueError as error:
        fail(str(error))
    (args.out / "notes.md").write_text(notes(args.version, build))

    print(f"Packaged {name} (build {build}) into {assets}:")
    for asset in sorted(assets.iterdir()):
        print(f"  {asset.name}  {asset.stat().st_size} bytes")
    github_output = os.environ.get("GITHUB_OUTPUT")
    if github_output:
        with open(github_output, "a") as f:
            f.write(f"build={build}\n")


if __name__ == "__main__":
    main()
