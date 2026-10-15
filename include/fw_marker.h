// fw_marker.h — firmware build-number marker (for OTA anti-downgrade).
//
// Every build embeds an FwBuildMarker (a magic tag + a monotonic build number)
// in .rodata. The OTA path streams the incoming image and scans it with
// BuildMarkerScanner to recover that build number WITHOUT trusting the image
// layout, then refuses any image whose build is older than the running floor.
//
// Framework-agnostic (no Arduino/ESP deps) so the scanner is host-testable.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace fogger {

// 8-byte magic that precedes the build number in the embedded marker. Chosen to
// be unique enough that it won't collide with ordinary image bytes.
constexpr char kFwMarkerMagic[8] = {'W', 'R', 'A', 'I', 'T', 'H', 'F', 'W'};

// Embedded in the firmware image; the 4-byte build follows the magic, little-
// endian (native layout on the C3). sizeof == 12, no padding.
struct FwBuildMarker {
  char magic[8];
  uint32_t build;
};

// Streams bytes, reports the first embedded build marker and counts every
// occurrence (a valid image carries exactly one; OTA rejects anything else, so a
// stray earlier "WRAITHFW" can't spoof the build). Keeps a rolling 12-byte
// window so markers are found even across chunk boundaries.
class BuildMarkerScanner {
 public:
  void reset() {
    win_ = 0;
    build_ = 0;
    count_ = 0;
  }

  void feed(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) push(data[i]);
  }

  bool found() const { return count_ > 0; }
  uint32_t build() const { return build_; }  // of the FIRST marker; 0 if none
  uint32_t count() const { return count_; }  // markers seen so far

 private:
  // Match the magic with immediate byte comparisons (not memcmp against a rodata
  // constant): that way the scanner emits no standalone "WRAITHFW" array that
  // the linker could constant-fold with the embedded marker's magic and detach
  // it from the build field. The embedded marker is then the ONLY WRAITHFW in
  // the image, with its build immediately after.
  static bool magicAt(const uint8_t* p) {
    return p[0] == 'W' && p[1] == 'R' && p[2] == 'A' && p[3] == 'I' &&
           p[4] == 'T' && p[5] == 'H' && p[6] == 'F' && p[7] == 'W';
  }

  void push(uint8_t b) {
    if (win_ < sizeof(buf_)) {
      buf_[win_++] = b;
    } else {
      memmove(buf_, buf_ + 1, sizeof(buf_) - 1);
      buf_[sizeof(buf_) - 1] = b;
    }
    if (win_ == sizeof(buf_) && magicAt(buf_)) {
      if (count_ == 0) {
        build_ = static_cast<uint32_t>(buf_[8]) |
                 (static_cast<uint32_t>(buf_[9]) << 8) |
                 (static_cast<uint32_t>(buf_[10]) << 16) |
                 (static_cast<uint32_t>(buf_[11]) << 24);
      }
      ++count_;
    }
  }

  uint8_t buf_[12];
  size_t win_ = 0;
  uint32_t build_ = 0;
  uint32_t count_ = 0;
};

}  // namespace fogger
