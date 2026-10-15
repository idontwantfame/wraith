// fw_version.cpp — embed the build-number marker into the image.
//
// The OTA path scans an incoming image for this marker to learn its build
// number and reject downgrades. The symbol is never referenced by code, so it
// is marked `used` to survive dead-code elimination, and `const` so it lands in
// .rodata (part of the streamed image).
#include "fw_version.h"

// Defined for this file only, by tools/build_number.py (see platformio.ini).
#ifndef WRAITH_FW_BUILD
#error "WRAITH_FW_BUILD is not set: build with PlatformIO (tools/build_number.py stamps it)"
#endif

namespace fogger {

constexpr uint32_t kFwBuild = WRAITH_FW_BUILD;

// External linkage + `used`, and referenced via fwBuild() from real code, so the
// linker keeps it in .rodata (part of the streamed OTA image). The magic bytes
// are written as literals and must match the scanner's magic in fw_marker.h.
__attribute__((used)) extern const FwBuildMarker g_fwBuildMarker = {
    {'W', 'R', 'A', 'I', 'T', 'H', 'F', 'W'}, kFwBuild};

static_assert(sizeof(FwBuildMarker) == 12, "marker must be 12 bytes, no padding");

}  // namespace fogger
