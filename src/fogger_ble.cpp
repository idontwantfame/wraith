// fogger_ble.cpp — NimBLE central implementation. See fogger_ble.h.
//
// API target: NimBLE-Arduino 2.x (required by Arduino-ESP32 3.x / IDF 5.x).
#include "fogger_ble.h"

#include <NimBLEDevice.h>

#include <algorithm>
#include <cctype>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace fogger {
namespace {

constexpr size_t kMaxCmdBytes = 8;       // longest writes: density and power-off (7)
constexpr uint32_t kScanRestartMs = 12000;  // re-arm scan if nothing is found
constexpr uint32_t kRssiPollMs = 2000;      // live-link signal refresh while connected
constexpr bool kWriteWithResponse = true;   // protocol uses Write Request

// Mutually-exclusive run intent (single atomic so the last writer wins).
constexpr uint8_t kRunNone = 0;
constexpr uint8_t kRunStart = 1;
constexpr uint8_t kRunStop = 2;
constexpr uint32_t kStartWriteTtlMs = 3000;  // drop a START that can't be written in time
constexpr int kStopBeforeDropAttempts = 2;    // STOP writes tried before a retarget drop
// The machine switched off ~7 s after the remote's power-off in the captures.
constexpr uint32_t kPowerOffWriteTtlMs = 3000;  // drop a power-off that can't be written in time
constexpr uint32_t kPowerOffHoldMs = 15000;     // refuse START/adjust this long after one

SemaphoreHandle_t asMutex(void* h) { return static_cast<SemaphoreHandle_t>(h); }
NimBLEClient* asClient(void* h) { return static_cast<NimBLEClient*>(h); }
NimBLERemoteCharacteristic* asChar(void* h) {
  return static_cast<NimBLERemoteCharacteristic*>(h);
}

std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

}  // namespace

const char* linkStateName(LinkState state) {
  switch (state) {
    case LinkState::kIdle: return "idle";
    case LinkState::kScanning: return "scanning";
    case LinkState::kConnecting: return "connecting";
    case LinkState::kConnected: return "connected";
  }
  return "unknown";
}

// --- NimBLE callback shims ---------------------------------------------------
// NimBLE invokes these on the host task. They do the minimum (match / flag) and
// hand off to the owning FoggerBle; all heavy work stays on the loop() thread.

class ScanShim : public NimBLEScanCallbacks {
 public:
  explicit ScanShim(FoggerBle* owner) : owner_(owner) {}
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    const std::string name = dev->getName();
    // A fog machine advertises the FFF0 service and/or the CH9141 name.
    const bool looksLikeMachine =
        dev->isAdvertisingService(NimBLEUUID(kServiceUuid)) ||
        name.rfind("CH9141", 0) == 0;
    owner_->handleAdvertisement(dev->getAddress().toString(), name,
                                looksLikeMachine, dev->getRSSI());
  }

 private:
  FoggerBle* owner_;
};

class ClientShim : public NimBLEClientCallbacks {
 public:
  explicit ClientShim(FoggerBle* owner) : owner_(owner) {}
  void onDisconnect(NimBLEClient* /*client*/, int /*reason*/) override {
    owner_->onBleDisconnect();  // just flag it; loop() does the teardown
  }

 private:
  FoggerBle* owner_;
};

// --- Lifecycle ---------------------------------------------------------------

bool FoggerBle::begin(const FoggerBleConfig& cfg) {
  if (started_) return true;
  cfg_ = cfg;
  targetMac_ = cfg_.targetMac;  // seed the live target from persisted config

  discoveredMux_ = xSemaphoreCreateMutex();
  targetMux_ = xSemaphoreCreateMutex();
  if (discoveredMux_ == nullptr || targetMux_ == nullptr) {
    log_e("fogger_ble: failed to create mutexes");
    return false;
  }

  if (!NimBLEDevice::init("")) {
    log_e("fogger_ble: NimBLE init failed");
    return false;  // BLE stays inert (loop() is a no-op until started_)
  }
  NimBLEScan* scan = NimBLEDevice::getScan();
  // Static shim: created once, lives for the process, never leaked.
  static ScanShim scanShim(this);
  scan->setScanCallbacks(&scanShim, /*wantDuplicates=*/false);
  scan->setActiveScan(true);
  // Duty-cycle the scan (~30%) so Wi-Fi gets radio time on the single-radio C3.
  // A full-duty continuous scan starves the softAP and blocks clients from
  // associating ("failed to join"). window < interval leaves gaps for Wi-Fi.
  scan->setInterval(100);
  scan->setWindow(30);

  started_ = true;
  link_.store(LinkState::kIdle);
  nextScanAtMs_ = millis();  // scan immediately on the first loop()
  log_i("fogger_ble: started, target=%s", targetMac_.c_str());
  return true;
}

void FoggerBle::loop() {
  if (!started_) return;
  applyPendingTarget();  // apply any setTarget() from another task, on this task
  // Run the teardown for an async drop here (single writer for loop-owned state).
  // Confirm the client really is down, so a stale callback (e.g. from a
  // deliberate disconnect) can't tear down a freshly re-established link.
  if (linkDropped_.exchange(false) && link_.load() != LinkState::kIdle &&
      (client_ == nullptr || !asClient(client_)->isConnected())) {
    handleDisconnect();
  }
  const uint32_t now = millis();

  switch (link_.load()) {
    case LinkState::kIdle:
      if ((int32_t)(now - nextScanAtMs_) >= 0) startScan();  // wrap-safe
      break;
    case LinkState::kScanning:
      if (foundTarget_.load()) {
        connectToFound();
      } else if (now - scanStartedMs_ > kScanRestartMs) {
        NimBLEDevice::getScan()->stop();
        startScan();  // recover from a silently-ended scan
      }
      break;
    case LinkState::kConnecting:
      break;  // connectToFound() resolves this synchronously
    case LinkState::kConnected:
      // The disconnect callback may not fire in every drop path; detect a dead
      // link here so we never sit "connected" on a stale handle.
      if (client_ == nullptr || !asClient(client_)->isConnected()) {
        handleDisconnect();
        break;
      }
      serviceCommands();
      pollLinkRssi(now);
      break;
  }

  // Publish newly-parsed frames from the loop thread (never from the BLE task).
  // The frame counter is read through snapshot(), i.e. under the telemetry lock.
  if (statusCb_) {
    const FoggerSnapshot snap = snapshot();
    if (snap.framesParsed != publishedFrames_) {
      publishedFrames_ = snap.framesParsed;
      statusCb_(snap);
    }
  }
}

// While connected, refresh the signal from the live link. The advertisement
// RSSI seen when connecting goes stale as soon as the link is up. This is a
// local query to the BLE controller (no radio traffic), so it doesn't delay
// commands.
void FoggerBle::pollLinkRssi(uint32_t now) {
  if ((int32_t)(now - nextRssiPollMs_) < 0) return;  // wrap-safe
  nextRssiPollMs_ = now + kRssiPollMs;
  const int rssi = asClient(client_)->getRssi();
  if (rssi == 0) return;  // query failed: keep the last value
  portENTER_CRITICAL(&mux_);
  rssi_ = rssi;
  portEXIT_CRITICAL(&mux_);
}

void FoggerBle::startScan() {
  link_.store(LinkState::kScanning);
  foundTarget_.store(false);
  scanStartedMs_ = millis();
  // duration 0 = scan until stopped (NimBLE 2.x: duration is in milliseconds).
  NimBLEDevice::getScan()->start(0, false);
  log_i("fogger_ble: scanning");
}

void FoggerBle::handleAdvertisement(const std::string& addr,
                                    const std::string& name,
                                    bool looksLikeMachine, int rssi) {
  if (looksLikeMachine) recordDiscovered(addr, name, rssi);

  // Match against the live target and record the connect address atomically, so
  // a target change on another task can't leave foundAddr_ pointing at the old
  // machine. Discover-only (never auto-connect) until a target is chosen.
  bool matched = false;
  if (xSemaphoreTake(asMutex(targetMux_), portMAX_DELAY) == pdTRUE) {
    if (!targetMac_.empty() && toLower(addr) == toLower(targetMac_)) {
      foundAddr_ = addr;
      matched = true;
    }
    xSemaphoreGive(asMutex(targetMux_));
  }
  if (!matched) return;

  foundTarget_.store(true);
  portENTER_CRITICAL(&mux_);
  rssi_ = rssi;
  portEXIT_CRITICAL(&mux_);
  NimBLEDevice::getScan()->stop();  // must be stopped before connecting
}

void FoggerBle::recordDiscovered(const std::string& mac, const std::string& rawName,
                                 int rssi) {
  constexpr size_t kMaxDiscovered = 16;
  constexpr size_t kMaxNameLen = 32;
  // Advertised names are attacker-controlled; bound the length we store.
  const std::string name =
      rawName.size() > kMaxNameLen ? rawName.substr(0, kMaxNameLen) : rawName;
  const std::string macLower = toLower(mac);
  const std::string target = toLower(targetMac());  // taken before discoveredMux_
  const uint32_t now = millis();
  if (xSemaphoreTake(asMutex(discoveredMux_), portMAX_DELAY) != pdTRUE) return;
  bool found = false;
  for (auto& d : discovered_) {
    if (toLower(d.mac) == macLower) {
      d.rssi = rssi;
      d.lastSeenMs = now;
      if (!name.empty()) d.name = name;
      found = true;
      break;
    }
  }
  bool added = false;
  if (!found && discovered_.size() < kMaxDiscovered) {
    discovered_.push_back({mac, name, rssi, now});
    added = true;
  } else if (!found) {
    // Full (e.g. flooded with spoofed CH9141 adverts): replace the stalest entry,
    // never the bound target, so a real machine can still reach the picker.
    auto victim = discovered_.end();
    for (auto it = discovered_.begin(); it != discovered_.end(); ++it) {
      if (toLower(it->mac) == target) continue;
      if (victim == discovered_.end() ||
          (int32_t)(it->lastSeenMs - victim->lastSeenMs) < 0) {
        victim = it;
      }
    }
    if (victim != discovered_.end()) {
      *victim = {mac, name, rssi, now};
      added = true;
    }
  }
  xSemaphoreGive(asMutex(discoveredMux_));
  if (added) {
    log_i("fogger_ble: discovered machine %s '%s' (%ddBm)", mac.c_str(),
          name.c_str(), rssi);
  }
}

std::vector<DiscoveredMachine> FoggerBle::discoveredMachines() const {
  std::vector<DiscoveredMachine> copy;
  if (xSemaphoreTake(asMutex(discoveredMux_), portMAX_DELAY) != pdTRUE) return copy;
  copy = discovered_;
  xSemaphoreGive(asMutex(discoveredMux_));
  return copy;
}

std::string FoggerBle::targetMac() const {
  std::string m;
  if (xSemaphoreTake(asMutex(targetMux_), portMAX_DELAY) == pdTRUE) {
    m = targetMac_;
    xSemaphoreGive(asMutex(targetMux_));
  }
  return m;
}

bool FoggerBle::setTarget(const std::string& mac) {
  // Marshal the change onto loop(); don't touch NimBLE/link state off-task.
  if (xSemaphoreTake(asMutex(targetMux_), portMAX_DELAY) != pdTRUE) return false;
  pendingTarget_ = mac;
  hasPendingTarget_ = true;
  xSemaphoreGive(asMutex(targetMux_));
  return true;
}

void FoggerBle::applyPendingTarget() {
  std::string mac, previous;
  bool has = false;
  if (xSemaphoreTake(asMutex(targetMux_), portMAX_DELAY) == pdTRUE) {
    if (hasPendingTarget_) {
      mac = pendingTarget_;
      previous = targetMac_;
      hasPendingTarget_ = false;
      targetMac_ = mac;  // the live target becomes the requested one
      has = true;
    }
    xSemaphoreGive(asMutex(targetMux_));
  }
  if (!has) return;
  // Re-selecting the machine we are already live on is a no-op: no needless
  // STOP + reconnect cycle.
  if (link_.load() == LinkState::kConnected && toLower(mac) == toLower(previous)) return;
  applyTarget(mac);
}

void FoggerBle::stopBeforeDrop() {
  // The protocol gives no guarantee that the machine stops on link loss
  // (docs/PROTOCOL.md), so never abandon a live link mid-fog: write
  // an explicit STOP first. Harmless if the machine is already idle.
  uint8_t buf[kMaxCmdBytes];
  const size_t len = buildFogStop(buf);
  for (int attempt = 0; attempt < kStopBeforeDropAttempts; ++attempt) {
    if (writeFrame(buf, len)) {
      uint8_t expected = kRunStop;  // a latched STOP has now been delivered
      runIntent_.compare_exchange_strong(expected, kRunNone);
      fogStopOwed_.store(false);
      return;
    }
  }
  // Keep a STOP latched so it is delivered on the next connect, not dropped.
  runIntent_.store(kRunStop);
  log_w("fogger_ble: STOP before dropping the link failed; latched, disconnecting");
}

void FoggerBle::inhibitStarts() {
  startsInhibited_.store(true);
  // A START latched just before this must not be written either: turn it into a
  // STOP (serviceCommands() also re-checks, closing the store-vs-check race).
  uint8_t expected = kRunStart;
  runIntent_.compare_exchange_strong(expected, kRunStop);
}

void FoggerBle::applyTarget(const std::string& mac) {
  foundTarget_.store(false);
  // Drop any pending ignite/adjust/power-off for the previous machine, but keep
  // a STOP.
  uint8_t start = kRunStart;
  runIntent_.compare_exchange_strong(start, kRunNone);
  densReq_.store(false);
  fanReq_.store(false);
  clearPowerOff();
  // Forget a stale matched address so a late advertisement can't reconnect the
  // old machine (reassembler is reset by handleDisconnect / connectToFound).
  if (xSemaphoreTake(asMutex(targetMux_), portMAX_DELAY) == pdTRUE) {
    foundAddr_.clear();
    xSemaphoreGive(asMutex(targetMux_));
  }
  if (link_.load() == LinkState::kConnected && client_ != nullptr) {
    stopBeforeDrop();                 // never leave the old machine spraying
    asClient(client_)->disconnect();  // ClientShim -> flag -> loop -> rescan
  } else {
    NimBLEDevice::getScan()->stop();
    link_.store(LinkState::kIdle);
    nextScanAtMs_ = millis();  // rescan immediately with the new target
  }
  log_i("fogger_ble: target set to '%s'", mac.c_str());
}

void FoggerBle::connectToFound() {
  link_.store(LinkState::kConnecting);
  foundTarget_.store(false);
  resetReassembler_.store(true);  // notify task resets before it next feeds
  NimBLEDevice::getScan()->stop();

  if (client_ == nullptr) {
    client_ = NimBLEDevice::createClient();
    // Static shim: created once, lives for the process, never leaked.
    static ClientShim clientShim(this);
    asClient(client_)->setClientCallbacks(&clientShim, /*deleteCB=*/false);
    // Bound the blocking connect() so loop() (and a pending STOP) can't stall on
    // the ~30 s NimBLE default when a machine is powered off mid-connect.
    asClient(client_)->setConnectTimeout(8000);
  }

  std::string connectAddr, target;
  if (xSemaphoreTake(asMutex(targetMux_), portMAX_DELAY) == pdTRUE) {
    connectAddr = foundAddr_;
    target = targetMac_;
    xSemaphoreGive(asMutex(targetMux_));
  }
  // The matched address must still be the current target (it can change between
  // the scan match and here); otherwise rescan for the new one.
  if (connectAddr.empty() || toLower(connectAddr) != toLower(target)) {
    link_.store(LinkState::kIdle);
    nextScanAtMs_ = millis();  // rescan immediately
    return;
  }

  NimBLEAddress addr(connectAddr, BLE_ADDR_PUBLIC);
  if (!asClient(client_)->connect(addr)) {
    log_w("fogger_ble: connect to %s failed", connectAddr.c_str());
    handleDisconnect();
    return;
  }

  if (!discoverCharacteristics()) {
    log_w("fogger_ble: service/characteristics not found; dropping link");
    asClient(client_)->disconnect();
    handleDisconnect();
    return;
  }

  // The link may have dropped during discovery: only publish "connected" if the
  // client is still up. The CAS keeps the kConnecting->kConnected transition
  // atomic with other paths; a drop during setup is caught here (isConnected)
  // and by the linkDropped_ flag on the next loop.
  // Backstop: no START/density/fan/power-off can have been legitimately latched
  // while the link was down (requests are connect-gated), so clear any stray
  // intent before we go live. A pending STOP is preserved.
  uint8_t start = kRunStart;
  runIntent_.compare_exchange_strong(start, kRunNone);
  densReq_.store(false);
  fanReq_.store(false);
  clearPowerOff();

  LinkState expected = LinkState::kConnecting;
  if (!asClient(client_)->isConnected() ||
      !link_.compare_exchange_strong(expected, LinkState::kConnected)) {
    log_w("fogger_ble: link dropped during setup; rescanning");
    asClient(client_)->disconnect();
    handleDisconnect();
    return;
  }
  log_i("fogger_ble: connected to %s", connectAddr.c_str());
}

bool FoggerBle::discoverCharacteristics() {
  NimBLERemoteService* svc = asClient(client_)->getService(kServiceUuid);
  if (svc == nullptr) return false;

  statusChar_ = svc->getCharacteristic(kStatusCharUuid);
  cmdChar_ = svc->getCharacteristic(kCommandCharUuid);
  if (statusChar_ == nullptr || cmdChar_ == nullptr) return false;

  if (!asChar(statusChar_)->canNotify()) return false;
  const bool ok = asChar(statusChar_)
                      ->subscribe(
                          /*notifications=*/true,
                          [this](NimBLERemoteCharacteristic*, uint8_t* data,
                                 size_t len, bool) { handleNotification(data, len); },
                          /*response=*/true);
  return ok;
}

void FoggerBle::handleDisconnect() {
  portENTER_CRITICAL(&mux_);
  const bool wasFogging = lastStatus_.isFogging();  // read before invalidating
  lastStatus_.valid = false;
  lastStatusMs_ = 0;
  portEXIT_CRITICAL(&mux_);
  cmdChar_ = nullptr;
  statusChar_ = nullptr;
  foundTarget_.store(false);
  link_.store(LinkState::kIdle);
  resetReassembler_.store(true);  // notify task discards any half-frame
  nextScanAtMs_ = millis() + cfg_.reconnectBackoffMs;
  // Stale ignite/adjust/power-off intents must not fire after reconnect; a STOP
  // is kept so it is delivered the moment the link comes back.
  uint8_t start = kRunStart;
  runIntent_.compare_exchange_strong(start, kRunNone);
  densReq_.store(false);
  fanReq_.store(false);
  clearPowerOff();
  // The machine may keep spraying with no link (the protocol never guarantees a
  // stop on link loss) and nothing can stop it meanwhile. If it was fogging,
  // latch a STOP so it is written the moment the link is back: a run is never
  // resumed blindly after an unexpected drop (programs already do this).
  if (wasFogging) {
    runIntent_.store(kRunStop);
    fogStopOwed_.store(true);
    log_w("fogger_ble: link lost while fogging; STOP latched for reconnect");
  }
  log_w("fogger_ble: disconnected; rescanning in %ums", cfg_.reconnectBackoffMs);
}

// --- Telemetry ---------------------------------------------------------------

void FoggerBle::handleNotification(const uint8_t* data, size_t len) {
  // Consume a reset requested by loop() here, so this task is the reassembler's
  // only writer (no cross-task race on its buffer).
  if (resetReassembler_.exchange(false)) reassembler_.reset();
  // Reassemble complete 20-byte frames; update shared state per valid frame.
  reassembler_.feed(data, len, [this](const StatusFrame& frame) {
    portENTER_CRITICAL(&mux_);
    lastStatus_ = frame;
    lastStatusMs_ = millis();
    ++framesParsed_;
    portEXIT_CRITICAL(&mux_);
  });
}

FoggerSnapshot FoggerBle::snapshot() const {
  FoggerSnapshot snap;
  portENTER_CRITICAL(&mux_);
  snap.rssi = rssi_;
  snap.lastStatusMs = lastStatusMs_;
  snap.framesParsed = framesParsed_;
  snap.status = lastStatus_;
  portEXIT_CRITICAL(&mux_);

  snap.link = link_.load();
  snap.connected = (snap.link == LinkState::kConnected);
  // Treat long-silent telemetry as not current (link may be up but mute).
  if (snap.lastStatusMs != 0 && millis() - snap.lastStatusMs > cfg_.staleStatusMs) {
    snap.status.valid = false;
  }
  return snap;
}

// --- Commands ----------------------------------------------------------------
// Requests only latch an intent; the GATT write happens in serviceCommands()
// from loop(). START and STOP are mutually exclusive (the later wins). Density
// and fan keep only the latest value. The return value reports link state; a
// latched STOP is still delivered once the link is (re)established.

bool FoggerBle::requestFogStart() {
  // Only latch START while connected, so a request made while the link is down
  // is NOT delivered on a later connect (no unattended ignition). Never while
  // shutting down (reboot / OTA / factory reset flush a STOP first), nor while
  // the machine is switching off.
  if (!connected() || startsInhibited_.load() || powerOffHeld()) return false;
  runIntentAtMs_.store(millis());
  runIntent_.store(kRunStart);
  return true;
}
bool FoggerBle::requestFogStop() {
  // STOP always latches (retried / delivered on reconnect) — safety.
  runIntent_.store(kRunStop);
  return connected();
}
bool FoggerBle::requestDensity(uint32_t density) {
  if (!connected() || startsInhibited_.load() || powerOffHeld()) return false;
  densVal_.store(density);
  densReq_.store(true);
  return true;
}
bool FoggerBle::requestFan(uint8_t fan) {
  if (!connected() || startsInhibited_.load() || powerOffHeld()) return false;
  fanVal_.store(static_cast<uint32_t>(fan));
  fanReq_.store(true);
  return true;
}
bool FoggerBle::requestPowerOff() {
  // Connect-gated like START: latched across a drop, it would switch the
  // machine off again the next time someone powers it on.
  if (!connected()) return false;
  const uint32_t now = millis();
  powerOffAtMs_.store(now);
  powerOffHoldUntilMs_.store((now + kPowerOffHoldMs) | 1);  // never 0 (= no hold)
  runIntent_.store(kRunStop);  // fog still ends if the power-off write never lands
  powerOffReq_.store(true);
  // The link may have dropped (and been torn down) since the check above: don't
  // leave a power-off waiting for the next connection. The STOP may stay.
  if (!connected()) {
    clearPowerOff();
    return false;
  }
  return true;
}

bool FoggerBle::powerOffHeld() const {
  // Signed, so a deadline stamped by another task a moment after our millis()
  // still reads as held. serviceCommands() clears it once expired, before the
  // signed difference could wrap back to positive.
  const uint32_t until = powerOffHoldUntilMs_.load();
  return until != 0 && (int32_t)(until - millis()) > 0;
}

void FoggerBle::clearPowerOff() {
  powerOffReq_.store(false);
  powerOffHoldUntilMs_.store(0);
}

bool FoggerBle::writeFrame(const uint8_t* buf, size_t len) {
  if (cmdChar_ == nullptr || link_.load() != LinkState::kConnected) return false;
  return asChar(cmdChar_)->writeValue(buf, len, kWriteWithResponse);
}

void FoggerBle::serviceCommands() {
  if (cmdChar_ == nullptr) return;
  uint8_t buf[kMaxCmdBytes];

  if (startsInhibited_.load()) {  // shutting down: a racing START becomes a STOP
    uint8_t start = kRunStart;
    runIntent_.compare_exchange_strong(start, kRunStop);
  }
  // Power-off first: its write carries the STOP. Like START it is dropped if it
  // can't be written in time. If its write fails, the STOP latched with it is
  // still tried below this pass.
  if (powerOffReq_.load()) {
    if ((int32_t)(millis() - powerOffAtMs_.load()) > (int32_t)kPowerOffWriteTtlMs) {
      clearPowerOff();
      log_w("fogger_ble: power-off not written in time; dropped (STOP still latched)");
    } else if (writeFrame(buf, buildPowerOff(buf))) {
      powerOffReq_.store(false);  // the hold stays until the link drops
      uint8_t expected = kRunStop;
      runIntent_.compare_exchange_strong(expected, kRunNone);
      fogStopOwed_.store(false);
      log_i("fogger_ble: power-off sent; the machine switches off in a few seconds");
      return;
    }
  }
  // While a power-off is held, this loop is the final arbiter: a START, density
  // or fan that slipped past the request-side check (racing requestPowerOff on
  // another task) is never written. The START becomes a STOP rather than being
  // dropped, since it may have overwritten the STOP latched with the power-off.
  const bool held = powerOffHeld();
  if (held) {
    uint8_t start = kRunStart;
    runIntent_.compare_exchange_strong(start, kRunStop);
  } else {
    // Clear an expired hold (CAS on the expired value, so a newer request isn't
    // lost); left set, the signed check would read held again ~24.9 days later.
    uint32_t until = powerOffHoldUntilMs_.load();
    if (until != 0 && (int32_t)(until - millis()) <= 0) {
      powerOffHoldUntilMs_.compare_exchange_strong(until, 0);
    }
  }

  const uint8_t intent = runIntent_.load();

  // STOP has priority and is retried: if its write fails, nothing else goes this
  // pass. Clear via CAS so a START arriving mid-write isn't lost.
  if (intent == kRunStop) {
    if (writeFrame(buf, buildFogStop(buf))) {
      uint8_t expected = kRunStop;
      runIntent_.compare_exchange_strong(expected, kRunNone);
      fogStopOwed_.store(false);  // the machine has its STOP now
    } else {
      return;
    }
  }

  // Density/fan: clear the latch BEFORE the write (exchange), and re-latch only
  // on failure, so a newer value set during the write is not dropped. Dropped
  // while a power-off is held.
  if (densReq_.exchange(false) && !held) {
    if (!writeFrame(buf, buildDensity(buf, densVal_.load()))) {
      densReq_.store(true);
      return;
    }
  }
  if (fanReq_.exchange(false) && !held) {
    if (!writeFrame(buf, buildFan(buf, static_cast<uint8_t>(fanVal_.load())))) {
      fanReq_.store(true);
      return;
    }
  }

  // START last, so it uses the latest density/fan. Apply only the intent we read
  // at the top (a STOP that arrived since must win, so don't re-read as START).
  if (intent == kRunStart && runIntent_.load() == kRunStart) {
    uint8_t expected = kRunStart;
    if (powerOffHeld()) {
      // A power-off arrived during this pass: never ignite; keep its STOP.
      runIntent_.compare_exchange_strong(expected, kRunStop);
    } else if ((int32_t)(millis() - runIntentAtMs_.load()) > (int32_t)kStartWriteTtlMs) {
      // Too stale (writes kept failing): drop it rather than ignite late.
      runIntent_.compare_exchange_strong(expected, kRunNone);
    } else if (writeFrame(buf, buildFogStart(buf))) {
      runIntent_.compare_exchange_strong(expected, kRunNone);
    }
  }
}

}  // namespace fogger
