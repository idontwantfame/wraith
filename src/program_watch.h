// program_watch.h — notices when the machine's real output disagrees with a
// running program's "on" pulse.
//
// The sequencer only sends START / STOP on a timer, but the machine can also
// stop by itself: its own button, or its protection. Without a check, the next
// pulse would restart fog that someone (or the machine) just stopped. During
// each on pulse, PulseWatch reads the machine's output-active status byte and
// tells the sequencer to give up when output stops early, or never starts.
//
// Limit: a stop can only be told apart from the program's own once the machine
// has had time to switch (kPulseSettleMs after the pulse starts) and before the
// program's own STOP at the end of the pulse. So a machine-button stop during a
// pulse shorter than about a second, or in the last moment of a pulse, is not
// recognised; the next pulse then starts as scheduled.
//
// Pure logic (no Arduino), so it is host-tested (test/test_program_watch).
#pragma once

#include <stdint.h>

namespace fogger {

// What the newest status frame says about the output. kUnknown when there is
// no fresh telemetry: the watch decides nothing on missing data (a lost link
// is handled separately, by aborting the program).
enum class OutputReading : uint8_t { kUnknown, kOff, kOn };

// One reading, with the millis() at which its status frame arrived.
struct OutputSample {
  OutputReading reading = OutputReading::kUnknown;
  uint32_t frameMs = 0;
};

enum class PulseVerdict : uint8_t {
  kContinue,        // nothing to do
  kStoppedOutside,  // output ran, then stopped without the program asking
  kDidNotStart,     // output never came on: the machine refused the pulse
};

// Ignore frames that arrived within this long of the pulse's START. The machine
// takes up to ~0.65 s to switch output (attended tests, 2026-10-21), so those
// frames can still show the previous state.
constexpr uint32_t kPulseSettleMs = 1000;
// A pulse whose output still reads off in a frame this long after its START
// was refused.
constexpr uint32_t kPulseStartGraceMs = 3000;
// A status frame older than this is not trusted to describe the output now
// (frames normally arrive every 0.1-0.25 s).
constexpr uint32_t kOutputFreshMs = 1500;

// Turn the BLE layer's latest telemetry into a sample.
inline OutputSample readOutput(bool connected, bool frameValid, bool outputActive,
                               uint32_t lastFrameMs, uint32_t nowMs) {
  if (!connected || !frameValid || lastFrameMs == 0) return {};
  if (nowMs - lastFrameMs > kOutputFreshMs) return {};
  return {outputActive ? OutputReading::kOn : OutputReading::kOff, lastFrameMs};
}

class PulseWatch {
 public:
  // Call when the program sends START for a pulse.
  void begin(uint32_t nowMs) {
    startMs_ = nowMs;
    seenOn_ = false;
    active_ = true;
  }
  // Call when the pulse ends (the program's own STOP, or the program stops).
  void end() { active_ = false; }

  PulseVerdict observe(const OutputSample& sample) {
    if (!active_ || sample.reading == OutputReading::kUnknown) return PulseVerdict::kContinue;
    // Judge by when the frame arrived: a frame from before this pulse, or from
    // the machine's switching lag, says nothing about it. Signed, so a frame
    // older than the pulse reads as negative rather than wrapping.
    const int32_t sinceStart = static_cast<int32_t>(sample.frameMs - startMs_);
    if (sinceStart < static_cast<int32_t>(kPulseSettleMs)) return PulseVerdict::kContinue;
    if (sample.reading == OutputReading::kOn) {
      seenOn_ = true;
      return PulseVerdict::kContinue;
    }
    if (seenOn_) return PulseVerdict::kStoppedOutside;
    return sinceStart >= static_cast<int32_t>(kPulseStartGraceMs) ? PulseVerdict::kDidNotStart
                                                                  : PulseVerdict::kContinue;
  }

 private:
  uint32_t startMs_ = 0;
  bool seenOn_ = false;
  bool active_ = false;
};

}  // namespace fogger
