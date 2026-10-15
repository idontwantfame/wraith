"""PlatformIO post-script: sign every firmware build, and `pio run -t ota`.

After each build of firmware.bin this signs it with tools/ota/sign.sh, so a
matching firmware.bin.sig always sits next to the image (every build gets a new
build number, so an older .sig would no longer match). Without the private key
it skips signing with a note: USB flashing doesn't need a signature.

`pio run -t ota` builds, signs and uploads the image over Wi-Fi to
custom_ota_host (platformio.ini), or to WRAITH_HOST=<ip or name.local> if set.
The device checks the signature and build number before installing, refuses
while fog is running, and reboots into the new firmware.
"""

import json
import os
import re
import subprocess
from pathlib import Path
from urllib.parse import quote

Import("env")  # noqa: F821 (injected by PlatformIO / SCons)

OTA_DIR = Path(env.subst("$PROJECT_DIR")) / "tools" / "ota"  # noqa: F821
SIGN_SCRIPT = OTA_DIR / "sign.sh"
PRIVATE_KEY = OTA_DIR / "ota_private_key.pem"  # path only; sign.sh reads it
FIRMWARE_BIN = "$BUILD_DIR/${PROGNAME}.bin"
DEFAULT_HOST = "wraith.local"
HOST_PATTERN = re.compile(r"^[A-Za-z0-9.-]+(:[0-9]{1,5})?$")  # name or IP, optional port
UPLOAD_TIMEOUT_S = 240  # the device allows 180 s per upload, plus slack
CSRF_HEADER = "X-Requested-With: fogger"  # the device requires it on every mutation
MARKER_MAGIC = b"WRAITHFW"  # fw_marker.h: magic + uint32 build, little-endian


def sign_image(image: Path) -> int:
    """Write <image>.sig with sign.sh. Returns a process exit code (0 = ok)."""
    return subprocess.run([str(SIGN_SCRIPT), str(image)], check=False).returncode


def sign_after_build(source, target, env) -> int:  # SCons action signature
    if not PRIVATE_KEY.is_file():
        print(f"OTA signing skipped: no {PRIVATE_KEY.name} in tools/ota (USB flashing doesn't need it)")
        return 0
    return sign_image(Path(target[0].get_abspath()))


def image_build(image: Path) -> str:
    data = image.read_bytes()
    at = data.find(MARKER_MAGIC)
    if at < 0:
        return "?"
    start = at + len(MARKER_MAGIC)
    return str(int.from_bytes(data[start:start + 4], "little"))


def ota_host(env) -> str:
    host = os.environ.get("WRAITH_HOST") or env.GetProjectOption("custom_ota_host", DEFAULT_HOST)
    return host.strip()


def ota_upload(source, target, env) -> int:  # SCons action signature
    image = Path(env.subst(FIRMWARE_BIN))
    host = ota_host(env)
    if not HOST_PATTERN.match(host):
        print(f"Error: '{host}' is not a host name or IP address (optionally with :port)")
        return 1
    if not PRIVATE_KEY.is_file():
        print(f"Error: OTA needs a signed image, but there is no {PRIVATE_KEY.name} in tools/ota")
        return 1
    if sign_image(image) != 0:  # sign this exact image, whatever happened before
        print("Error: signing failed")
        return 1

    sig = quote(Path(f"{image}.sig").read_text().strip(), safe="")
    print(f"Uploading build {image_build(image)} to http://{host}/ (the device reboots when done)...")
    result = subprocess.run(
        ["curl", "--silent", "--show-error", "--max-time", str(UPLOAD_TIMEOUT_S),
         "-H", CSRF_HEADER, "-F", f"firmware=@{image}", f"http://{host}/api/ota?sig={sig}"],
        capture_output=True, text=True, check=False,
    )
    if result.returncode != 0:
        print(f"Error: upload failed: {result.stderr.strip()}")
        return 1
    try:
        reply = json.loads(result.stdout)
    except ValueError:
        print(f"Error: unexpected reply from the device: {result.stdout[:200]!r}")
        return 1
    if not reply.get("ok"):
        print(f"Error: the device refused the update: {reply.get('error', 'no reason given')}")
        return 1
    print("Update accepted; the device is rebooting into it.")
    return 0


env.AddPostAction(FIRMWARE_BIN, sign_after_build)  # noqa: F821
env.AddCustomTarget(  # noqa: F821
    name="ota",
    dependencies=FIRMWARE_BIN,
    actions=[ota_upload],
    title="OTA upload",
    description="Build, sign and upload the firmware over Wi-Fi",
)
