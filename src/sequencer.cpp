// sequencer.cpp — timed fog programs. See sequencer.h.
#include "sequencer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace fogger {
namespace {
constexpr uint32_t kMinOnMs = 200;
constexpr uint32_t kMaxOnMs = 60000;        // 60 s max per pulse (safety)
constexpr uint32_t kMinOffMs = 200;
constexpr uint32_t kMaxOffMs = 3600000;     // 1 h max gap
constexpr uint16_t kMaxRepeats = 1000;
constexpr uint32_t kMaxProgramMs = 2 * 3600 * 1000UL;  // 2 h hard runtime cap

uint32_t clampU32(uint32_t v, uint32_t lo, uint32_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

SemaphoreHandle_t asMutex(void* h) { return static_cast<SemaphoreHandle_t>(h); }

// Minimal RAII scoped lock over the FreeRTOS mutex (no-op if not created yet).
class Lock {
 public:
  explicit Lock(void* m) : m_(m) {
    if (m_) xSemaphoreTake(asMutex(m_), portMAX_DELAY);
  }
  ~Lock() {
    if (m_) xSemaphoreGive(asMutex(m_));
  }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;

 private:
  void* m_;
};
}  // namespace

const char* seqPhaseName(SeqPhase phase) {
  switch (phase) {
    case SeqPhase::kIdle: return "idle";
    case SeqPhase::kOn: return "on";
    case SeqPhase::kOff: return "off";
  }
  return "unknown";
}

void Sequencer::wire(Action fogStart, Action fogStop, DensityAction density,
                     FanAction fan, Predicate connected) {
  fogStart_ = std::move(fogStart);
  fogStop_ = std::move(fogStop);
  density_ = std::move(density);
  fan_ = std::move(fan);
  connected_ = std::move(connected);
  if (mutex_ == nullptr) mutex_ = xSemaphoreCreateMutex();
}

bool Sequencer::start(const FogProgram& program) {
  Lock lock(mutex_);
  if (!fogStart_ || !fogStop_) return false;
  // Don't start a program against a machine that isn't connected: the first
  // START would fail and later pulses would re-fire blindly on reconnect.
  if (connected_ && !connected_()) return false;

  prog_ = program;
  prog_.onMs = clampU32(prog_.onMs, kMinOnMs, kMaxOnMs);
  prog_.offMs = clampU32(prog_.offMs, kMinOffMs, kMaxOffMs);
  if (prog_.repeats > kMaxRepeats) prog_.repeats = kMaxRepeats;

  if (prog_.density != 0 && density_) density_(prog_.density);
  if (prog_.fan != 0 && fan_) fan_(prog_.fan);

  running_ = true;
  cycle_ = 1;
  startMs_ = millis();
  enterOnLocked();
  log_i("seq: start on=%ums off=%ums repeats=%u", prog_.onMs, prog_.offMs,
        prog_.repeats);
  return true;
}

void Sequencer::enterOnLocked() {
  phase_ = SeqPhase::kOn;
  if (fogStart_) fogStart_();
  phaseEndMs_ = millis() + prog_.onMs;
}

void Sequencer::enterOffLocked() {
  phase_ = SeqPhase::kOff;
  if (fogStop_) fogStop_();
  phaseEndMs_ = millis() + prog_.offMs;
}

void Sequencer::stopLocked() {
  if (fogStop_) fogStop_();
  running_ = false;
  phase_ = SeqPhase::kIdle;
  log_i("seq: stopped");
}

void Sequencer::stop() {
  Lock lock(mutex_);
  stopLocked();
}

void Sequencer::cancel() {
  Lock lock(mutex_);
  running_ = false;
  phase_ = SeqPhase::kIdle;
}

void Sequencer::loop() {
  Lock lock(mutex_);
  if (!running_) return;

  // If the machine link dropped mid-program, abort (latches a STOP) instead of
  // blindly re-firing fog when it comes back.
  if (connected_ && !connected_()) {
    log_w("seq: link lost; stopping program");
    stopLocked();
    return;
  }

  const uint32_t now = millis();

  // Hard safety cap on total runtime, regardless of the program.
  if (now - startMs_ > kMaxProgramMs) {
    log_w("seq: hit max runtime cap; stopping");
    stopLocked();
    return;
  }

  if ((int32_t)(now - phaseEndMs_) < 0) return;  // current phase still running

  if (phase_ == SeqPhase::kOn) {
    // Finished a pulse. If that was the last one, stop (no trailing off).
    if (prog_.repeats != 0 && cycle_ >= prog_.repeats) {
      stopLocked();
      return;
    }
    enterOffLocked();
  } else if (phase_ == SeqPhase::kOff) {
    ++cycle_;
    enterOnLocked();
  }
}

bool Sequencer::running() const {
  Lock lock(mutex_);
  return running_;
}

SeqStatus Sequencer::status() const {
  Lock lock(mutex_);
  SeqStatus s;
  s.running = running_;
  s.phase = phase_;
  s.cycle = cycle_;
  s.repeats = prog_.repeats;
  if (running_) {
    const uint32_t now = millis();
    s.phaseRemainingMs = (int32_t)(phaseEndMs_ - now) > 0 ? (phaseEndMs_ - now) : 0;
    s.elapsedMs = now - startMs_;
  }
  return s;
}

}  // namespace fogger
