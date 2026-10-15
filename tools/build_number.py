"""PlatformIO pre-script: stamp the firmware build number (WRAITH_FW_BUILD).

OTA anti-downgrade needs a number that rises with every build. It is the UTC
build time as yymmddHHMM (2610151452 = 2026-10-15 14:52 UTC): readable, always
increasing, and it fits the 32-bit build field of the image marker
(fw_marker.h) until the end of 2042. UTC, so a daylight-saving change can't
make it go backwards.

Set the WRAITH_FW_BUILD environment variable to build a specific number
instead (e.g. to reproduce a released image byte for byte).

Only src/fw_version.cpp gets the define, so a new number recompiles that one
file and relinks, instead of rebuilding the whole project.
"""

import os
import time

Import("env")  # noqa: F821 (injected by PlatformIO / SCons)

BUILD_TIME_FORMAT = "%y%m%d%H%M"
MAX_BUILD = 0xFFFFFFFF  # FwBuildMarker::build is a uint32_t
STAMPED_SOURCE = "*fw_version.cpp"


def build_number() -> int:
    override = os.environ.get("WRAITH_FW_BUILD", "").strip()
    text = override or time.strftime(BUILD_TIME_FORMAT, time.gmtime())
    if not text.isdigit() or not 0 < int(text) <= MAX_BUILD:
        print(f"Error: WRAITH_FW_BUILD must be a whole number from 1 to {MAX_BUILD}, got '{text}'")
        env.Exit(1)  # noqa: F821
    return int(text)


BUILD = build_number()


def stamp_build(build_env, node):
    return build_env.Object(node, CCFLAGS=build_env["CCFLAGS"] + [f"-DWRAITH_FW_BUILD={BUILD}"])


env.AddBuildMiddleware(stamp_build, STAMPED_SOURCE)  # noqa: F821
print(f"Wraith firmware build {BUILD}")
