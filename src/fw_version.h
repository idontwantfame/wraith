// fw_version.h — the firmware build number (monotonic, for OTA anti-downgrade).
//
// Stamped automatically on every build by tools/build_number.py (the UTC build
// time as yymmddHHMM), so OTA can refuse to install an older image (see
// ota.cpp). Only fw_version.cpp sees the WRAITH_FW_BUILD define.
#pragma once

#include <stdint.h>

#include "fw_marker.h"

namespace fogger {
// The marker physically embedded in the image (see fw_version.cpp). Declared
// here so code can READ it: that reference is what keeps the linker from
// garbage-collecting the marker out of the image (`used` alone is not enough
// against --gc-sections).
extern const FwBuildMarker g_fwBuildMarker;

// The running build number, read from the embedded marker (so the symbol is
// referenced and retained).
inline uint32_t fwBuild() { return g_fwBuildMarker.build; }
}  // namespace fogger
