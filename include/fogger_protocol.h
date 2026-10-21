// fogger_protocol.h — verified wire protocol for the Filmog / E-TIMING machine.
//
// Framework-agnostic (no Arduino/ESP deps) so it ports to ESP-IDF or host tests.
// Single source of truth; mirrors docs/PROTOCOL.md. All command and status facts
// here come from BLE captures and attended tests on the real machine.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace fogger {

// ---- GATT identifiers -------------------------------------------------------
// Machine advertises as "CH9141BLE2U". Service holds FFF1 (notify) + FFF2 (write).
constexpr char kDefaultMac[] = "70:19:88:B1:22:50";  // public address
constexpr char kServiceUuid[] = "0000fff0-0000-1000-8000-00805f9b34fb";
constexpr char kStatusCharUuid[] = "0000fff1-0000-1000-8000-00805f9b34fb";   // notify
constexpr char kCommandCharUuid[] = "0000fff2-0000-1000-8000-00805f9b34fb";  // write

// ---- Command framing --------------------------------------------------------
// Commands are `55 aa <id> [payload]`. No checksum/terminator/pairing required.
constexpr uint8_t kCmdHdr0 = 0x55;
constexpr uint8_t kCmdHdr1 = 0xAA;

enum CmdId : uint8_t {
  kCmdFogStart = 0x00,  // verified: fog + pump + fan on
  kCmdFogStop = 0x02,   // verified: all off
  kCmdDensity = 0x04,   // payload: u32 big-endian
  kCmdFan = 0x05,       // payload: value, second-param (0x64)
  // 0x03 01 is the remote's switch-off request; only sent inside buildPowerOff().
  // 0x03 00 cancels a pending switch-off: never sent, so the machine's own
  // power button keeps working while we are connected.
  kCmdPower = 0x03,
};
constexpr uint8_t kPowerOffArg = 0x01;

// Verified useful ranges (remote min/middle/max).
constexpr uint32_t kDensityMin = 8000;
constexpr uint32_t kDensityMid = 25000;
constexpr uint32_t kDensityMax = 40000;
constexpr uint8_t kFanMin = 35;
constexpr uint8_t kFanMax = 100;
constexpr uint8_t kFanSecondParam = 0x64;  // 100; kept constant in all tests

inline uint32_t clampU32(uint32_t v, uint32_t lo, uint32_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
inline uint8_t clampU8(uint8_t v, uint8_t lo, uint8_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// Command builders. Each writes into `out` (>= 7 bytes) and returns the length.
inline size_t buildFogStart(uint8_t* out) {
  out[0] = kCmdHdr0; out[1] = kCmdHdr1; out[2] = kCmdFogStart;
  return 3;
}
inline size_t buildFogStop(uint8_t* out) {
  out[0] = kCmdHdr0; out[1] = kCmdHdr1; out[2] = kCmdFogStop;
  return 3;
}
inline size_t buildDensity(uint8_t* out, uint32_t density) {
  density = clampU32(density, kDensityMin, kDensityMax);
  out[0] = kCmdHdr0; out[1] = kCmdHdr1; out[2] = kCmdDensity;
  out[3] = (density >> 24) & 0xFF;  // big-endian
  out[4] = (density >> 16) & 0xFF;
  out[5] = (density >> 8) & 0xFF;
  out[6] = density & 0xFF;
  return 7;
}
inline size_t buildFan(uint8_t* out, uint8_t fan) {
  fan = clampU8(fan, kFanMin, kFanMax);
  out[0] = kCmdHdr0; out[1] = kCmdHdr1; out[2] = kCmdFan;
  out[3] = fan;
  out[4] = kFanSecondParam;
  return 5;
}
// What the original remote writes when it is switched off: STOP, then 03 01,
// in one write. The machine switches itself off a few seconds later and can
// then only be powered on with its own button.
inline size_t buildPowerOff(uint8_t* out) {
  const size_t stopLen = buildFogStop(out);
  out[stopLen] = kCmdHdr0; out[stopLen + 1] = kCmdHdr1;
  out[stopLen + 2] = kCmdPower; out[stopLen + 3] = kPowerOffArg;
  return stopLen + 4;
}

// ---- Status frame -----------------------------------------------------------
// 20 bytes: `aa 55 14 ... ff ff ff`. Notifications may batch/split frames, so a
// StreamReassembler (below) finds frame boundaries in the byte stream.
constexpr uint8_t kStatusHdr0 = 0xAA;
constexpr uint8_t kStatusHdr1 = 0x55;
constexpr uint8_t kStatusLen = 0x14;  // 20
constexpr size_t kFrameSize = 20;
constexpr uint8_t kStatusTrailer = 0xFF;  // frame ends with three 0xFF bytes
constexpr uint8_t kMaxBatteryPct = 100;   // telemetry over 100% is invalid

struct StatusFrame {
  uint8_t run03 = 0;         // offset 3: command-03 field (NOT physical fog run)
  uint32_t density = 0;      // offsets 4..7, big-endian
  uint8_t fan = 0;           // offset 8
  uint8_t fanParam = 0;      // offset 9
  uint16_t faultMirror = 0;  // offsets 10..11, big-endian (report mirror)
  uint8_t batteryA = 0;      // offset 12, percent
  uint8_t batteryB = 0;      // offset 13, percent
  uint8_t chargerPresent = 0;// offset 14 (0 absent / 1 connected)
  uint8_t chargeComplete = 0;// offset 15
  uint8_t outputActive = 0;  // offset 16: the real fog-run indicator (1 = spraying)
  bool valid = false;

  // True when the machine is physically producing fog.
  bool isFogging() const { return valid && outputActive != 0; }
  uint8_t batteryMin() const { return batteryA < batteryB ? batteryA : batteryB; }
};

// Parse exactly 20 bytes starting at a verified header. Returns an invalid frame
// unless BOTH the `aa 55 14` header and the trailing `ff ff ff` are present, so
// a frame merged across a lost notification chunk is rejected rather than
// surfaced as a plausible-but-wrong state (e.g. a false "fogging" readout).
inline StatusFrame parseFrame(const uint8_t* f) {
  StatusFrame s;
  if (f[0] != kStatusHdr0 || f[1] != kStatusHdr1 || f[2] != kStatusLen) return s;
  if (f[17] != kStatusTrailer || f[18] != kStatusTrailer || f[19] != kStatusTrailer)
    return s;
  s.run03 = f[3];
  s.density = (uint32_t(f[4]) << 24) | (uint32_t(f[5]) << 16) |
              (uint32_t(f[6]) << 8) | uint32_t(f[7]);
  s.fan = f[8];
  s.fanParam = f[9];
  s.faultMirror = (uint16_t(f[10]) << 8) | f[11];
  s.batteryA = f[12] > kMaxBatteryPct ? kMaxBatteryPct : f[12];
  s.batteryB = f[13] > kMaxBatteryPct ? kMaxBatteryPct : f[13];
  s.chargerPresent = f[14];
  s.chargeComplete = f[15];
  s.outputActive = f[16];
  s.valid = true;
  return s;
}

// Reassembles 20-byte frames from a chunked notification stream. Scans for the
// `aa 55 14` header and emits one StatusFrame per complete frame.
class StreamReassembler {
 public:
  // Feed a notification chunk; calls sink(frame) for each complete frame found.
  template <typename Sink>
  void feed(const uint8_t* data, size_t len, Sink&& sink) {
    for (size_t i = 0; i < len; ++i) push(data[i], sink);
  }

  // Drop any partial frame. Call on (dis)connect so bytes from one link can
  // never be glued onto the next link's stream.
  void reset() { count_ = 0; }

 private:
  template <typename Sink>
  void push(uint8_t b, Sink& sink) {
    // Header sync (count_ 0..2). On any mismatch, re-evaluate THIS byte as a
    // fresh header start rather than dropping it — otherwise a real frame whose
    // `aa` directly follows a false `aa 55` start would be swallowed.
    if (count_ == 0) {
      if (b == kStatusHdr0) buf_[count_++] = b;  // start of a candidate header
      return;
    }
    if (count_ == 1) {
      if (b == kStatusHdr1) buf_[count_++] = b;        // aa 55
      else if (b != kStatusHdr0) count_ = 0;           // not a header; drop
      return;                                          // (aa again: keep buf_[0])
    }
    if (count_ == 2) {
      if (b == kStatusLen) buf_[count_++] = b;         // aa 55 14 — locked
      else if (b == kStatusHdr0) count_ = 1;           // restart on a new aa
      else count_ = 0;                                 // false start; resync
      return;
    }
    // Locked onto a header: accumulate the fixed-size body.
    buf_[count_++] = b;
    if (count_ == kFrameSize) {
      StatusFrame s = parseFrame(buf_);
      if (s.valid) {
        sink(s);
        count_ = 0;
        return;
      }
      // The 20 bytes were not a valid frame (e.g. a chunk was lost and two
      // frames merged). Don't discard the whole window: replay bytes 1..19 so a
      // genuine `aa 55 14` header starting mid-buffer is still found. Replaying
      // at most 19 bytes can never re-complete a 20-byte frame here, so this
      // does not recurse.
      uint8_t tail[kFrameSize - 1];
      memcpy(tail, buf_ + 1, kFrameSize - 1);
      count_ = 0;
      for (size_t i = 0; i < kFrameSize - 1; ++i) push(tail[i], sink);
    }
  }
  uint8_t buf_[kFrameSize];
  size_t count_ = 0;
};

}  // namespace fogger
