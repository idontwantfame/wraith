// test_protocol.cpp — host unit tests for the framework-agnostic protocol core.
//
// fogger_protocol.h has no Arduino/ESP dependency, so it compiles and runs on
// the dev host. Run with `pio test -e native -e native_tls` (see
// test/test_custom_runner.py).
//
// Covers every verified command builder, the status-frame parser (offsets +
// derived flags), and the notification StreamReassembler (split / batched /
// resync / reject). Keep in lockstep with docs/PROTOCOL.md.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "fogger_protocol.h"
#include "fw_marker.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
      ++g_failures;                                                       \
    }                                                                     \
  } while (0)

// Build a valid 20-byte status frame from field values for parser tests.
std::vector<uint8_t> makeFrame(uint8_t run03, uint32_t density, uint8_t fan,
                               uint8_t fanParam, uint16_t fault, uint8_t battA,
                               uint8_t battB, uint8_t charger, uint8_t chargeDone,
                               uint8_t outputActive) {
  std::vector<uint8_t> f(fogger::kFrameSize, 0);
  f[0] = fogger::kStatusHdr0;
  f[1] = fogger::kStatusHdr1;
  f[2] = fogger::kStatusLen;
  f[3] = run03;
  f[4] = (density >> 24) & 0xFF;
  f[5] = (density >> 16) & 0xFF;
  f[6] = (density >> 8) & 0xFF;
  f[7] = density & 0xFF;
  f[8] = fan;
  f[9] = fanParam;
  f[10] = (fault >> 8) & 0xFF;
  f[11] = fault & 0xFF;
  f[12] = battA;
  f[13] = battB;
  f[14] = charger;
  f[15] = chargeDone;
  f[16] = outputActive;
  f[17] = f[18] = f[19] = 0xFF;
  return f;
}

void testCommandBuilders() {
  uint8_t buf[8] = {0};

  CHECK(fogger::buildFogStart(buf) == 3);
  CHECK(buf[0] == 0x55 && buf[1] == 0xAA && buf[2] == 0x00);

  CHECK(fogger::buildFogStop(buf) == 3);
  CHECK(buf[0] == 0x55 && buf[1] == 0xAA && buf[2] == 0x02);

  // Density: big-endian u32, clamped to [8000, 40000].
  CHECK(fogger::buildDensity(buf, 25000) == 7);
  CHECK(buf[2] == 0x04);
  uint32_t d = (uint32_t(buf[3]) << 24) | (uint32_t(buf[4]) << 16) |
               (uint32_t(buf[5]) << 8) | buf[6];
  CHECK(d == 25000);

  fogger::buildDensity(buf, 100);  // below min -> 8000
  d = (uint32_t(buf[3]) << 24) | (uint32_t(buf[4]) << 16) |
      (uint32_t(buf[5]) << 8) | buf[6];
  CHECK(d == fogger::kDensityMin);

  fogger::buildDensity(buf, 999999);  // above max -> 40000
  d = (uint32_t(buf[3]) << 24) | (uint32_t(buf[4]) << 16) |
      (uint32_t(buf[5]) << 8) | buf[6];
  CHECK(d == fogger::kDensityMax);

  // Fan: value clamped to [35, 100], second param constant 0x64.
  CHECK(fogger::buildFan(buf, 70) == 5);
  CHECK(buf[2] == 0x05 && buf[3] == 70 && buf[4] == 0x64);
  fogger::buildFan(buf, 10);
  CHECK(buf[3] == fogger::kFanMin);
  fogger::buildFan(buf, 200);
  CHECK(buf[3] == fogger::kFanMax);
}

void testParseFrame() {
  auto good = makeFrame(1, 30000, 80, 0x64, 0x0000, 90, 85, 1, 0, 1);
  fogger::StatusFrame s = fogger::parseFrame(good.data());
  CHECK(s.valid);
  CHECK(s.run03 == 1);
  CHECK(s.density == 30000);
  CHECK(s.fan == 80);
  CHECK(s.batteryA == 90 && s.batteryB == 85);
  CHECK(s.batteryMin() == 85);
  CHECK(s.chargerPresent == 1 && s.chargeComplete == 0);
  CHECK(s.outputActive == 1);
  CHECK(s.isFogging());  // offset 16 drives this, NOT run03

  // run03 set but output inactive -> not fogging (the whole point of the fix).
  auto standby = makeFrame(1, 30000, 80, 0x64, 0, 90, 85, 1, 0, /*output=*/0);
  fogger::StatusFrame st = fogger::parseFrame(standby.data());
  CHECK(st.valid);
  CHECK(st.run03 == 1);
  CHECK(!st.isFogging());

  // Bad header -> invalid, and never reports fogging.
  auto bad = makeFrame(0, 0, 0, 0, 0, 0, 0, 0, 0, 1);
  bad[0] = 0x00;
  fogger::StatusFrame b = fogger::parseFrame(bad.data());
  CHECK(!b.valid);
  CHECK(!b.isFogging());

  // Missing trailer (a frame merged across a lost chunk) -> invalid.
  auto noTrailer = makeFrame(0, 30000, 80, 0x64, 0, 90, 85, 1, 0, 1);
  noTrailer[19] = 0x00;
  CHECK(!fogger::parseFrame(noTrailer.data()).valid);

  // Battery readings above 100% are clamped (invalid telemetry, not surfaced raw).
  auto overBatt = makeFrame(0, 10000, 50, 0x64, 0, 200, 150, 0, 0, 0);
  fogger::StatusFrame ob = fogger::parseFrame(overBatt.data());
  CHECK(ob.batteryA == 100 && ob.batteryB == 100);
}

// Collect frames the reassembler emits for a given byte stream.
std::vector<fogger::StatusFrame> runReassembler(const std::vector<uint8_t>& bytes,
                                                 size_t chunk) {
  fogger::StreamReassembler r;
  std::vector<fogger::StatusFrame> out;
  for (size_t i = 0; i < bytes.size(); i += chunk) {
    size_t n = std::min(chunk, bytes.size() - i);
    r.feed(&bytes[i], n, [&](const fogger::StatusFrame& f) { out.push_back(f); });
  }
  return out;
}

void testReassembler() {
  auto a = makeFrame(0, 10000, 50, 0x64, 0, 70, 70, 0, 0, 0);
  auto b = makeFrame(1, 40000, 100, 0x64, 0, 60, 55, 1, 1, 1);

  // Single whole frame.
  auto one = runReassembler(a, fogger::kFrameSize);
  CHECK(one.size() == 1 && one[0].density == 10000);

  // Same frame fed one byte at a time.
  auto drip = runReassembler(a, 1);
  CHECK(drip.size() == 1 && drip[0].fan == 50);

  // Two frames batched in one buffer.
  std::vector<uint8_t> batched(a.begin(), a.end());
  batched.insert(batched.end(), b.begin(), b.end());
  auto two = runReassembler(batched, batched.size());
  CHECK(two.size() == 2);
  CHECK(two[0].density == 10000 && two[1].density == 40000);
  CHECK(two[1].isFogging());

  // Two frames split at an awkward 7-byte chunk boundary.
  auto split = runReassembler(batched, 7);
  CHECK(split.size() == 2 && split[1].outputActive == 1);

  // Leading garbage must be skipped; the real frame still parses (resync).
  std::vector<uint8_t> noisy = {0x00, 0x12, 0xAA, 0xAA, 0x55};  // false starts
  noisy.insert(noisy.end(), b.begin(), b.end());
  auto resynced = runReassembler(noisy, 3);
  CHECK(resynced.size() == 1 && resynced[0].density == 40000);

  // A dropped byte mid-frame must not emit a merged, wrong frame; the next whole
  // frame after the damage still parses (resync on trailer-reject).
  std::vector<uint8_t> damaged(a.begin(), a.end() - 1);  // frame a, last FF lost
  damaged.insert(damaged.end(), b.begin(), b.end());     // followed by whole b
  auto recovered = runReassembler(damaged, damaged.size());
  CHECK(recovered.size() == 1 && recovered[0].density == 40000);  // only b, not a

  // reset() drops a partial frame so it cannot glue onto a later stream.
  fogger::StreamReassembler r;
  std::vector<fogger::StatusFrame> got;
  auto sink = [&](const fogger::StatusFrame& f) { got.push_back(f); };
  r.feed(a.data(), 10, sink);  // half of frame a
  r.reset();                   // e.g. a disconnect
  r.feed(b.data(), b.size(), sink);
  CHECK(got.size() == 1 && got[0].density == 40000);
}

// Append the 12-byte build marker (magic + u32 LE build) to a buffer.
void appendMarker(std::vector<uint8_t>& out, uint32_t build) {
  for (char c : fogger::kFwMarkerMagic) out.push_back((uint8_t)c);
  out.push_back(build & 0xFF);
  out.push_back((build >> 8) & 0xFF);
  out.push_back((build >> 16) & 0xFF);
  out.push_back((build >> 24) & 0xFF);
}

void testBuildMarker() {
  // Marker embedded in surrounding noise is found, with the right build.
  std::vector<uint8_t> img = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
  appendMarker(img, 1234);
  img.insert(img.end(), {0x66, 0x77, 0x88});
  fogger::BuildMarkerScanner s;
  s.feed(img.data(), img.size());
  CHECK(s.found());
  CHECK(s.build() == 1234);

  // Found even when the marker straddles a chunk boundary (1 byte at a time).
  fogger::BuildMarkerScanner drip;
  for (uint8_t b : img) drip.feed(&b, 1);
  CHECK(drip.found() && drip.build() == 1234);

  // No marker -> not found, build 0.
  std::vector<uint8_t> none = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
  fogger::BuildMarkerScanner s2;
  s2.feed(none.data(), none.size());
  CHECK(!s2.found());
  CHECK(s2.build() == 0);

  // Large build value round-trips (little-endian).
  std::vector<uint8_t> big;
  appendMarker(big, 0x01020304u);
  fogger::BuildMarkerScanner s3;
  s3.feed(big.data(), big.size());
  CHECK(s3.found() && s3.build() == 0x01020304u);

  // Every occurrence is counted, so OTA can require exactly one: a stray earlier
  // "WRAITHFW" must not be able to spoof the build number past the floor.
  CHECK(s.count() == 1);
  CHECK(s2.count() == 0);
  std::vector<uint8_t> two;
  appendMarker(two, 9999);  // spoofed earlier marker
  two.insert(two.end(), {0x01, 0x02, 0x03});
  appendMarker(two, 7);  // the real one
  fogger::BuildMarkerScanner s4;
  s4.feed(two.data(), two.size());
  CHECK(s4.count() == 2);
  CHECK(s4.build() == 9999);  // first reported; the caller rejects count() != 1

  // Counting also works across 1-byte chunks.
  fogger::BuildMarkerScanner s5;
  for (uint8_t b : two) s5.feed(&b, 1);
  CHECK(s5.count() == 2);

  // reset() clears the count.
  s4.reset();
  CHECK(s4.count() == 0 && !s4.found());
}

}  // namespace

int main() {
  testCommandBuilders();
  testParseFrame();
  testReassembler();
  testBuildMarker();
  if (g_failures == 0) {
    std::printf("OK: all protocol tests passed\n");
    return 0;
  }
  std::printf("%d assertion(s) failed\n", g_failures);
  return 1;
}
