// sequencer.h — timed fog programs ("modes": pulse N times, on X / off Y).
//
// Drives the BLE command layer on a timer: e.g. "fog 5s, off 5s, repeat 5".
// Runs entirely from loop(); never auto-starts (must be started explicitly).
//
// Safety: per-pulse on-time is capped, and a whole program is force-stopped
// after a hard maximum runtime so a mistaken "infinite" program cannot run
// away. stop() always sends a fog STOP.
#pragma once

#include <Arduino.h>

#include <functional>

namespace fogger {

struct FogProgram {
  uint32_t onMs = 5000;    // fog on per pulse
  uint32_t offMs = 5000;   // gap between pulses
  uint16_t repeats = 5;    // number of pulses; 0 = infinite (until stopped/cap)
  uint32_t density = 0;    // applied once at start; 0 = leave unchanged
  uint8_t fan = 0;         // applied once at start; 0 = leave unchanged
};

enum class SeqPhase : uint8_t { kIdle, kOn, kOff };
const char* seqPhaseName(SeqPhase phase);

struct SeqStatus {
  bool running = false;
  SeqPhase phase = SeqPhase::kIdle;
  uint16_t cycle = 0;             // 1-based current pulse
  uint16_t repeats = 0;           // 0 = infinite
  uint32_t phaseRemainingMs = 0;
  uint32_t elapsedMs = 0;
};

class Sequencer {
 public:
  using Action = std::function<bool()>;
  using DensityAction = std::function<bool(uint32_t)>;
  using FanAction = std::function<bool(uint8_t)>;
  using Predicate = std::function<bool()>;

  // Wire the command callbacks. `connected` reports whether the machine link is
  // up, so the program can refuse to start, and abort rather than blindly
  // re-fire, when there is no link. Creates the internal lock; call once.
  void wire(Action fogStart, Action fogStop, DensityAction density, FanAction fan,
            Predicate connected);

  // Start a (clamped) program. Returns false if commands are not wired or the
  // machine is not connected (so the UI reports an honest failure).
  bool start(const FogProgram& program);
  // Stop the program and send a fog STOP.
  void stop();
  // Stop timing WITHOUT sending a command (manual/MQTT control took over).
  void cancel();
  void loop();

  bool running() const;
  SeqStatus status() const;

 private:
  // State mutators that assume the caller already holds mutex_.
  void enterOnLocked();
  void enterOffLocked();
  void stopLocked();

  FogProgram prog_;
  bool running_ = false;
  SeqPhase phase_ = SeqPhase::kIdle;
  uint16_t cycle_ = 0;
  uint32_t phaseEndMs_ = 0;
  uint32_t startMs_ = 0;

  // Guards all mutable state: web handlers call start/stop/cancel on the async
  // task while loop() advances the program on the main task. void* so this
  // header stays free of FreeRTOS includes (cast in the .cpp).
  void* mutex_ = nullptr;

  Action fogStart_;
  Action fogStop_;
  DensityAction density_;
  FanAction fan_;
  Predicate connected_;
};

}  // namespace fogger
