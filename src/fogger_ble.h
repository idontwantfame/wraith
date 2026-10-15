// fogger_ble.h — NimBLE central client for the Filmog / E-TIMING fog machine.
//
// Owns the BLE link lifecycle (scan -> connect -> discover -> subscribe ->
// reconnect) and exposes a thread-safe snapshot of the latest telemetry plus
// thread-safe command requests. All wire details live in fogger_protocol.h.
//
// Threading: NimBLE host callbacks run on the NimBLE task; command requests may
// arrive from other tasks (e.g. the async web server). Telemetry is guarded by a
// portMUX critical section; the live target by a FreeRTOS mutex; command requests
// are lock-free atomic intents. The disconnect callback only raises a flag. All
// GATT writes and link-state changes happen on loop(), the single owner.
#pragma once

#include <Arduino.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "fogger_protocol.h"

namespace fogger {

// Link lifecycle, surfaced to the UI so it can show "searching / connecting /
// live" rather than a bare connected/disconnected bit.
enum class LinkState : uint8_t { kIdle, kScanning, kConnecting, kConnected };

const char* linkStateName(LinkState state);

// Immutable value snapshot handed to callers; copied out under the lock so the
// caller never reads a half-updated frame.
struct FoggerSnapshot {
  LinkState link = LinkState::kIdle;
  bool connected = false;
  int rssi = 0;               // last advertisement RSSI (dBm), 0 if unknown
  uint32_t lastStatusMs = 0;  // millis() when the last valid frame arrived, 0 if never
  uint32_t framesParsed = 0;  // total valid status frames since begin()
  StatusFrame status;         // last parsed telemetry (see .valid)
};

struct FoggerBleConfig {
  std::string targetMac = kDefaultMac;  // machine public address; "" = discover only
  uint32_t reconnectBackoffMs = 2000;   // wait between a drop and the next scan
  uint32_t staleStatusMs = 5000;        // mark telemetry stale after this gap
};

// A machine seen advertising the fog service during scanning (for the picker).
struct DiscoveredMachine {
  std::string mac;
  std::string name;
  int rssi = 0;
  uint32_t lastSeenMs = 0;  // millis() of the latest advertisement (eviction)
};

// One BLE link to one machine. Not copyable (owns NimBLE resources).
class FoggerBle {
 public:
  using StatusCallback = std::function<void(const FoggerSnapshot&)>;

  FoggerBle() = default;
  FoggerBle(const FoggerBle&) = delete;
  FoggerBle& operator=(const FoggerBle&) = delete;

  // Initialise NimBLE and begin searching for the configured machine. Returns
  // false if the locks or the NimBLE stack could not be initialised (BLE then
  // stays inert). Safe to call once.
  bool begin(const FoggerBleConfig& cfg);

  // Drive the link state machine and drain the command queue. Call from loop().
  void loop();

  // Thread-safe copy of the latest link + telemetry state.
  FoggerSnapshot snapshot() const;

  // Command requests. Thread-safe; the actual GATT write happens in loop().
  // These latch an intent (never block, never drop): the return value reports
  // whether the link is currently up, but a latched STOP is still delivered as
  // soon as the link is (re)established. START/STOP are mutually exclusive (the
  // later one wins); density/fan keep only the latest value (coalesced), so a
  // slider drag cannot starve a STOP.
  bool requestFogStart();
  bool requestFogStop();
  bool requestDensity(uint32_t density);
  bool requestFan(uint8_t fan);

  // One-way: refuse every later START/density/fan (any task) and turn a latched
  // START into a STOP. Called before a reboot so nothing (MQTT task, web) can
  // ignite during the final STOP flush.
  void inhibitStarts();

  // True while the GATT link to the machine is up.
  bool connected() const { return link_.load() == LinkState::kConnected; }

  // True from a link drop while the machine was fogging until the STOP latched
  // for it has been written (the machine may still be spraying unseen).
  // Thread-safe.
  bool stopOwed() const { return fogStopOwed_.load(); }

  // Invoked from loop() after each new valid status frame (not from the BLE
  // callback), so handlers may do non-trivial work safely.
  void onStatus(StatusCallback cb) { statusCb_ = std::move(cb); }

  // Machines seen advertising the fog service during scanning. Thread-safe copy.
  std::vector<DiscoveredMachine> discoveredMachines() const;

  // Bind to a different machine at runtime (the caller persists it). Forces a
  // reconnect to the new target; an empty mac means discover-only (no connect).
  // Safe to call from any task: the change is applied on the loop() thread.
  bool setTarget(const std::string& mac);

  // The live target MAC. Thread-safe (guarded; read from the web task).
  std::string targetMac() const;

  // --- Internal hooks for NimBLE callback shims (public for the shim classes,
  // not part of the intended caller API). ---
  void handleAdvertisement(const std::string& addr, const std::string& name,
                           bool looksLikeMachine, int rssi);
  // Called from the NimBLE host task on a drop: only raises a flag; loop() runs
  // the actual teardown (handleDisconnect) so loop-owned state has one writer.
  void onBleDisconnect() { linkDropped_.store(true); }
  void handleNotification(const uint8_t* data, size_t len);

 private:
  void startScan();
  void connectToFound();
  bool discoverCharacteristics();
  void applyPendingTarget();                 // consume a setTarget() from loop()
  void applyTarget(const std::string& mac);  // loop()-thread target switch
  void stopBeforeDrop();                     // write STOP before abandoning a live link
  void handleDisconnect();                   // link teardown (loop() thread only)
  void serviceCommands();                    // write latched intents (loop only)
  bool writeFrame(const uint8_t* buf, size_t len);  // one GATT write
  void recordDiscovered(const std::string& mac, const std::string& name, int rssi);

  FoggerBleConfig cfg_;
  bool started_ = false;

  // Link state machine (loop() thread, except link_ which is read from other
  // tasks via connected()/snapshot(), hence atomic).
  std::atomic<LinkState> link_{LinkState::kIdle};
  std::atomic<bool> foundTarget_{false};  // scan matched; connect on next loop()
  uint32_t nextScanAtMs_ = 0;     // backoff gate for the next scan attempt
  uint32_t scanStartedMs_ = 0;    // when the current scan began (restart guard)
  uint32_t publishedFrames_ = 0;  // framesParsed_ already handed to statusCb_

  // Latched command intents, serviced in loop() (GATT writes only from loop()).
  // START and STOP are a single atomic (kRun* in the .cpp) so the last writer
  // wins even when requests interleave across tasks; STOP is retried until it
  // writes. Density/fan keep only the latest value. A START/density/fan is only
  // latched while connected; a STOP latches unconditionally (fires on reconnect).
  std::atomic<uint8_t> runIntent_{0};  // kRunNone / kRunStart / kRunStop
  std::atomic<uint32_t> runIntentAtMs_{0};  // when a START was latched (write TTL)
  std::atomic<bool> densReq_{false};
  std::atomic<bool> fanReq_{false};
  std::atomic<bool> startsInhibited_{false};  // set once by inhibitStarts()
  std::atomic<bool> fogStopOwed_{false};      // see stopOwed()
  std::atomic<uint32_t> densVal_{0};
  std::atomic<uint32_t> fanVal_{0};

  // Set by the NimBLE host task's disconnect callback; the real cleanup runs on
  // loop() so loop-owned state has a single writer.
  std::atomic<bool> linkDropped_{false};
  // Requested by loop() on (dis)connect, consumed by the notify task before it
  // feeds, so the reassembler has exactly one writer (the host task).
  std::atomic<bool> resetReassembler_{false};

  // Live target + the connect address + a pending setTarget(), all guarded by
  // targetMux_ because they are touched by the web task, the NimBLE host task
  // and loop(). foundAddr_ is written by the host task, read by loop().
  void* targetMux_ = nullptr;     // SemaphoreHandle_t
  std::string targetMac_;         // the machine we want to be bound to
  std::string foundAddr_;         // address a scan matched (to connect to)
  std::string pendingTarget_;     // a setTarget() awaiting loop() application
  bool hasPendingTarget_ = false;

  // Shared telemetry, written by the notify callback (NimBLE task), read by
  // snapshot(). portMUX is used because the writer path must stay allocation-
  // free; only fixed-size POD is touched here.
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  int rssi_ = 0;
  uint32_t lastStatusMs_ = 0;
  uint32_t framesParsed_ = 0;
  StatusFrame lastStatus_;

  // Discovered-machines list involves heap (std::string/vector), so it uses a
  // FreeRTOS mutex rather than portMUX (never touched from an ISR).
  std::vector<DiscoveredMachine> discovered_;
  void* discoveredMux_ = nullptr;  // SemaphoreHandle_t

  StreamReassembler reassembler_;
  StatusCallback statusCb_;

  // NimBLE handles are kept as opaque pointers so this header stays light; the
  // .cpp casts them back. (NimBLEClient* / NimBLERemoteCharacteristic*.)
  void* client_ = nullptr;
  void* cmdChar_ = nullptr;
  void* statusChar_ = nullptr;
};

}  // namespace fogger
