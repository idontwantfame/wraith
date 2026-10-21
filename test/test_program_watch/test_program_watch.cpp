// test_program_watch.cpp — host unit tests for the program pulse watch: a
// running program must give up when the machine stops output on its own
// (machine button, protection) or refuses to start a pulse, and must never
// act on stale, early or missing telemetry.
//
// Run: pio test -e native -e native_tls
#include <cstdint>
#include <cstdio>

#include "program_watch.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
      ++g_failures;                                                       \
    }                                                                     \
  } while (0)

using fogger::kOutputFreshMs;
using fogger::kPulseSettleMs;
using fogger::kPulseStartGraceMs;
using fogger::OutputReading;
using fogger::OutputSample;
using fogger::PulseVerdict;
using fogger::PulseWatch;
using fogger::readOutput;

// A fresh frame that arrived at `frameMs`.
OutputSample on(uint32_t frameMs) { return {OutputReading::kOn, frameMs}; }
OutputSample off(uint32_t frameMs) { return {OutputReading::kOff, frameMs}; }
const OutputSample kUnknown{};

void testReadOutput() {
  const uint32_t now = 100000;
  const OutputSample a = readOutput(true, true, true, now - 100, now);
  CHECK(a.reading == OutputReading::kOn && a.frameMs == now - 100);
  CHECK(readOutput(true, true, false, now - 100, now).reading == OutputReading::kOff);
  CHECK(readOutput(false, true, true, now - 100, now).reading == OutputReading::kUnknown);  // link down
  CHECK(readOutput(true, false, true, now - 100, now).reading == OutputReading::kUnknown);  // invalid
  CHECK(readOutput(true, true, true, 0, now).reading == OutputReading::kUnknown);           // never received
  CHECK(readOutput(true, true, false, now - kOutputFreshMs, now).reading == OutputReading::kOff);
  CHECK(readOutput(true, true, false, now - kOutputFreshMs - 1, now).reading ==
        OutputReading::kUnknown);  // stale
  // millis() wrap: a frame 0x200 ms before a wrapped clock is still fresh.
  CHECK(readOutput(true, true, true, 0xFFFFFF00u, 0x00000100u).reading == OutputReading::kOn);
}

void testNormalPulse() {
  PulseWatch w;
  w.begin(0);
  CHECK(w.observe(off(300)) == PulseVerdict::kContinue);  // switching lag
  CHECK(w.observe(off(kPulseSettleMs + 100)) == PulseVerdict::kContinue);
  CHECK(w.observe(on(kPulseSettleMs + 300)) == PulseVerdict::kContinue);
  CHECK(w.observe(on(5000)) == PulseVerdict::kContinue);
  // The program's own STOP ends the pulse: later off frames mean nothing.
  w.end();
  CHECK(w.observe(off(5200)) == PulseVerdict::kContinue);
  CHECK(w.observe(off(20000)) == PulseVerdict::kContinue);
}

void testStoppedOutside() {
  PulseWatch w;
  w.begin(1000);
  CHECK(w.observe(on(2500)) == PulseVerdict::kContinue);
  CHECK(w.observe(off(4000)) == PulseVerdict::kStoppedOutside);  // machine button
}

void testStopRightAfterSettle() {
  PulseWatch w;
  w.begin(0);
  CHECK(w.observe(on(kPulseSettleMs)) == PulseVerdict::kContinue);  // first judged frame
  CHECK(w.observe(off(kPulseSettleMs)) == PulseVerdict::kStoppedOutside);
}

void testDidNotStart() {
  PulseWatch w;
  w.begin(0);
  CHECK(w.observe(off(kPulseSettleMs)) == PulseVerdict::kContinue);
  CHECK(w.observe(off(kPulseStartGraceMs - 1)) == PulseVerdict::kContinue);
  CHECK(w.observe(off(kPulseStartGraceMs)) == PulseVerdict::kDidNotStart);
}

void testEarlyFramesIgnored() {
  // A short off phase: frames early in the pulse can still show the previous
  // pulse's output (on), then the machine's stop of it (off), then the new
  // START taking effect (on). None of that may arm or decide anything.
  PulseWatch w;
  w.begin(0);
  CHECK(w.observe(on(100)) == PulseVerdict::kContinue);
  CHECK(w.observe(off(500)) == PulseVerdict::kContinue);
  CHECK(w.observe(on(900)) == PulseVerdict::kContinue);
  // Not armed by the early "on": an off frame after settle is no outside stop.
  CHECK(w.observe(off(kPulseSettleMs + 1)) == PulseVerdict::kContinue);
}

void testFrameOlderThanPulseIgnored() {
  // Judged by frame arrival time, not "now": a frame from before START (still
  // fresh by age) says nothing about this pulse, even observed late.
  PulseWatch w;
  w.begin(10000);
  CHECK(w.observe(on(9800)) == PulseVerdict::kContinue);   // previous pulse's output
  CHECK(w.observe(off(9900)) == PulseVerdict::kContinue);  // before START: not "refused"
  // The same stale "on" must not have armed it: an off after settle is not an
  // outside stop, and only becomes "did not start" after the grace period.
  CHECK(w.observe(off(10000 + kPulseSettleMs)) == PulseVerdict::kContinue);
  CHECK(w.observe(off(10000 + kPulseStartGraceMs)) == PulseVerdict::kDidNotStart);
}

void testShortPulseNeverJudged() {
  // A 200 ms pulse ends (the program's own STOP) before any frame is judged.
  PulseWatch w;
  w.begin(0);
  CHECK(w.observe(on(150)) == PulseVerdict::kContinue);
  CHECK(w.observe(off(190)) == PulseVerdict::kContinue);
  w.end();
  CHECK(w.observe(off(kPulseStartGraceMs + 100)) == PulseVerdict::kContinue);
}

void testUnknownDecidesNothing() {
  PulseWatch w;
  w.begin(0);
  CHECK(w.observe(kUnknown) == PulseVerdict::kContinue);
  CHECK(w.observe(on(kPulseStartGraceMs + 100)) == PulseVerdict::kContinue);
  CHECK(w.observe(kUnknown) == PulseVerdict::kContinue);
  CHECK(w.observe(off(kPulseStartGraceMs + 300)) == PulseVerdict::kStoppedOutside);
}

void testRestart() {
  PulseWatch w;
  CHECK(w.observe(off(10000)) == PulseVerdict::kContinue);  // never begun
  w.begin(0);
  CHECK(w.observe(on(2000)) == PulseVerdict::kContinue);    // armed
  w.end();
  w.begin(10000);  // next pulse forgets the previous pulse's "on"
  CHECK(w.observe(off(10000 + kPulseSettleMs + 10)) == PulseVerdict::kContinue);
  // begin() again without end() also starts over.
  w.begin(20000);
  CHECK(w.observe(off(20000 + kPulseSettleMs)) == PulseVerdict::kContinue);
  CHECK(w.observe(off(20000 + kPulseStartGraceMs)) == PulseVerdict::kDidNotStart);
}

void testClockWrap() {
  PulseWatch w;
  w.begin(0xFFFFFF00u);
  CHECK(w.observe(on(0xFFFFFF00u + 100)) == PulseVerdict::kContinue);  // settling
  CHECK(w.observe(on(0x00000400u)) == PulseVerdict::kContinue);        // 1280 ms: armed
  CHECK(w.observe(off(0x00000500u)) == PulseVerdict::kStoppedOutside);
}

}  // namespace

int main() {
  testReadOutput();
  testNormalPulse();
  testStoppedOutside();
  testStopRightAfterSettle();
  testDidNotStart();
  testEarlyFramesIgnored();
  testFrameOlderThanPulseIgnored();
  testShortPulseNeverJudged();
  testUnknownDecidesNothing();
  testRestart();
  testClockWrap();
  if (g_failures == 0) {
    std::printf("OK: all program watch tests passed\n");
    return 0;
  }
  std::printf("%d assertion(s) failed\n", g_failures);
  return 1;
}
