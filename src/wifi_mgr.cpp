// wifi_mgr.cpp — STA + captive-portal fallback. See wifi_mgr.h.
#include "wifi_mgr.h"

#include <ESPmDNS.h>
#include <esp_random.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace fogger {
namespace {
constexpr uint8_t kDnsPort = 53;
constexpr int kScanRunning = -1;   // WiFi.scanComplete() while in progress
constexpr int kScanFailed = -2;    // WiFi.scanComplete() after a failed/timed-out scan
constexpr uint8_t kMaxStaAttempts = 3;  // retry STA this many times before portal

std::string randomHex(size_t n) {
  static const char kHex[] = "0123456789abcdef";
  std::string s;
  s.reserve(n);
  for (size_t i = 0; i < n; ++i) s += kHex[esp_random() & 0x0F];
  return s;
}

SemaphoreHandle_t asMutex(void* h) { return static_cast<SemaphoreHandle_t>(h); }

// RAII scoped lock over the cfg mutex (no-op before begin() creates it).
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

const char* wifiModeName(WifiMode mode) {
  switch (mode) {
    case WifiMode::kBooting: return "booting";
    case WifiMode::kConnecting: return "connecting";
    case WifiMode::kStaConnected: return "connected";
    case WifiMode::kApPortal: return "portal";
  }
  return "unknown";
}

bool WifiManager::begin(const WifiConfig& cfg) {
  if (cfgMux_ == nullptr) cfgMux_ = xSemaphoreCreateMutex();
  cfg_ = cfg;
  goodSsid_ = cfg.ssid;  // the saved creds are known-good until proven otherwise
  goodPass_ = cfg.pass;
  WiFi.persistent(false);       // we own credential storage (NVS via Config)
  WiFi.setAutoReconnect(false);  // we drive (re)connects; don't silently rejoin
  // Join the strongest access point of the network. The core's default fast
  // scan takes the first AP found with the SSID (channels in order), which on
  // a multi-AP network can be a far one; sorting by signal needs the full scan.
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

  // Debug visibility: log the key Wi-Fi events (AP client join/leave, scan
  // done, STA associate / got-IP / disconnect-with-reason).
  WiFi.onEvent([](arduino_event_id_t e, arduino_event_info_t info) {
    switch (e) {
      case ARDUINO_EVENT_WIFI_AP_STACONNECTED: log_i("wifi/ev: AP client joined"); break;
      case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: log_i("wifi/ev: AP client left"); break;
      case ARDUINO_EVENT_WIFI_SCAN_DONE: log_i("wifi/ev: scan done"); break;
      case ARDUINO_EVENT_WIFI_STA_CONNECTED: log_i("wifi/ev: STA associated"); break;
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        log_i("wifi/ev: STA got IP %s", WiFi.localIP().toString().c_str());
        break;
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        log_w("wifi/ev: STA disconnected, reason=%d",
              info.wifi_sta_disconnected.reason);
        break;
      default: break;
    }
  });

  if (!cfg_.ssid.empty()) {
    startSta();
  } else {
    startPortal();
  }
  return true;
}

void WifiManager::startSta(bool keepApDuringConnect) {
  std::string ssid, pass;
  {
    Lock lock(cfgMux_);
    ssid = cfg_.ssid;
    pass = cfg_.pass;
  }
  // AP_STA keeps the captive portal (and the operator's phone session) alive
  // while we try the new credentials, so onboarding doesn't drop mid-connect.
  WiFi.mode(keepApDuringConnect ? WIFI_AP_STA : WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  mode_.store(WifiMode::kConnecting);
  staDeadlineMs_ = millis() + cfg_.staConnectTimeoutMs;
  log_i("wifi: connecting to '%s'%s", ssid.c_str(),
        keepApDuringConnect ? " (portal kept up)" : "");
}

void WifiManager::startPortal() {
  std::string apName, apPass, ssid;
  {
    Lock lock(cfgMux_);
    apName = cfg_.apName;
    apPass = cfg_.apPass;
    ssid = cfg_.ssid;
  }
  // AP_STA (not plain AP) so a Wi-Fi scan runs on the station interface and the
  // softAP keeps serving the operator's phone instead of dropping it when the
  // radio sweeps other channels. Only (re)start the AP if it is not already up,
  // so a background STA retry that fails does not kick connected clients.
  WiFi.mode(WIFI_AP_STA);
  if (!apUp_) {
    // Fail closed: never run an open AP. If Config has no usable password (NVS
    // failure, or a corrupt value below WPA2's 8-char minimum), use a random one
    // for this boot and print it to serial.
    if (apPass.size() < 8) {
      apPass = randomHex(16);
      log_w("wifi: no usable AP password; random for this boot: %s", apPass.c_str());
    }
    WiFi.softAP(apName.c_str(), apPass.c_str());  // WPA2
    dns_.start(kDnsPort, "*", WiFi.softAPIP());  // redirect every lookup to us
    apUp_ = true;
  }
  mode_.store(WifiMode::kApPortal);
  needScan_.store(true);  // scan now, before a client joins, and cache the result
  nextRetryMs_ = ssid.empty() ? 0 : millis() + cfg_.staRetryMs;
  log_i("wifi: portal '%s' at %s", apName.c_str(),
        WiFi.softAPIP().toString().c_str());
}

void WifiManager::stopPortal() {
  if (!apUp_) return;
  dns_.stop();
  WiFi.softAPdisconnect(/*wifioff=*/true);  // drop the AP interface entirely
  WiFi.mode(WIFI_STA);                       // station-only once on the LAN
  apUp_ = false;
  log_i("wifi: portal torn down (STA only)");
}

void WifiManager::setHostname(const std::string& name) {
  {
    Lock lock(cfgMux_);
    cfg_.hostname = name.empty() ? cfg_.apName : name;  // "" -> auto (device id)
  }
  // Called on the web task: MDNS.end()/begin() must not race loop()'s own
  // startMdns(), so only flag it; loop() re-registers while connected.
  pendingMdns_.store(true);
}

std::string WifiManager::hostname() const {
  Lock lock(cfgMux_);
  return cfg_.hostname;
}

void WifiManager::startMdns() {
  std::string host;
  {
    Lock lock(cfgMux_);
    host = cfg_.hostname;
  }
  MDNS.end();  // safe if not started; lets us re-register after a reconnect
  if (MDNS.begin(host.c_str())) {
    MDNS.addService("http", "tcp", 80);
    log_i("wifi: mDNS up -> http://%s.local", host.c_str());
  } else {
    log_w("wifi: mDNS start failed");
  }
}

void WifiManager::loop() {
  applyPendingConnect();  // a tryConnect() from the web task, run on its owner
  switch (mode_.load()) {
    case WifiMode::kBooting:
      break;
    case WifiMode::kConnecting:
      if (WiFi.status() == WL_CONNECTED) {
        staAttempts_ = 0;
        stopPortal();  // never leave the onboarding AP serving the full API
        mode_.store(WifiMode::kStaConnected);
        log_i("wifi: connected, ip=%s", WiFi.localIP().toString().c_str());
        pendingMdns_.store(false);  // this registration uses the latest name
        startMdns();
        // Promote to known-good + persist ONLY if we actually associated with the
        // candidate SSID (guards a stale WL_CONNECTED from an earlier link racing
        // a just-set candidate, which would persist an unproven network).
        const std::string assoc = std::string(WiFi.SSID().c_str());
        bool matches = false;
        {
          Lock lock(cfgMux_);
          matches = (assoc == cfg_.ssid);
          if (matches) {
            goodSsid_ = cfg_.ssid;
            goodPass_ = cfg_.pass;
          }
        }
        // Only a match consumes the persist flag: on a mismatch the candidate is
        // still unproven, and must be saved if it connects on a later attempt.
        if (matches) {
          if (credsDirty_ && onConnected_) onConnected_(goodSsid_, goodPass_);
          credsDirty_ = false;
        }
      } else if ((int32_t)(millis() - staDeadlineMs_) >= 0) {
        bool haveCreds;
        {
          Lock lock(cfgMux_);
          haveCreds = !cfg_.ssid.empty();
        }
        if (haveCreds && ++staAttempts_ < kMaxStaAttempts) {
          log_w("wifi: STA timeout (%u/%u); retrying", staAttempts_, kMaxStaAttempts);
          startSta(/*keepApDuringConnect=*/apUp_);  // don't drop a live portal
        } else {
          // An unproven candidate failed: restore the known-good creds so portal
          // retries (and any reconnect) use the saved network, not the bad one.
          {
            Lock lock(cfgMux_);
            cfg_.ssid = goodSsid_;
            cfg_.pass = goodPass_;
          }
          credsDirty_ = false;
          log_w("wifi: STA connect failed; starting portal");
          startPortal();
          staAttempts_ = 0;
        }
      }
      break;
    case WifiMode::kStaConnected:
      if (WiFi.status() != WL_CONNECTED) {
        log_w("wifi: STA link lost; reconnecting");
        startSta();  // retried with backoff; portal only after kMaxStaAttempts
        break;
      }
      if (pendingMdns_.exchange(false)) startMdns();  // hostname changed live
      serviceScan();  // on the LAN, scans run only when the UI requests one
      break;
    case WifiMode::kApPortal:
      dns_.processNextRequest();
      serviceScan();  // runs the pre-scan once (before clients) and caches it
      // Periodically re-attempt saved STA creds without a client having to act.
      if (nextRetryMs_ != 0 && (int32_t)(millis() - nextRetryMs_) >= 0) {
        log_i("wifi: retrying saved STA from portal");
        startSta(/*keepApDuringConnect=*/true);
      }
      break;
  }
}

WifiStatus WifiManager::status() const {
  const WifiMode m = mode_.load();
  WifiStatus s;
  s.mode = m;
  s.portal = (m == WifiMode::kApPortal);
  Lock lock(cfgMux_);
  if (m == WifiMode::kStaConnected) {
    s.ssid = WiFi.SSID().c_str();  // the network actually joined, not a candidate
    s.ip = WiFi.localIP().toString().c_str();
    s.rssi = WiFi.RSSI();
    // Which access point: with several APs on one SSID, a far one explains a
    // weak RSSI and slow replies.
    s.bssid = WiFi.BSSIDstr().c_str();
    s.channel = WiFi.channel();
  } else if (m == WifiMode::kApPortal) {
    s.ssid = cfg_.apName;
    s.ip = WiFi.softAPIP().toString().c_str();
  } else {
    s.ssid = cfg_.ssid;
  }
  return s;
}

void WifiManager::startScan(bool force) {
  // Auto-opens reuse the cache (a live re-scan drops the softAP client). The
  // explicit Scan button passes force=true to re-scan anyway.
  bool haveCache;
  {
    Lock lock(cfgMux_);
    haveCache = !cachedNetworks_.empty();
  }
  if (scanPending_.load()) return;  // one already in flight
  if (force) {
    needScan_.store(true);
    return;
  }
  // Never auto-scan on the LAN: a scan takes the radio off-channel and stalls
  // every HTTP reply for ~10 s, so simply opening Settings would delay loading
  // the saved settings (a save over that half-filled form wiped MQTT creds).
  // Only the portal auto-scans (its pre-scan runs before a client joins).
  if (!haveCache && mode_.load() == WifiMode::kApPortal) needScan_.store(true);
}

bool WifiManager::scanRunning() const { return scanPending_.load() || needScan_.load(); }

std::vector<WifiNetwork> WifiManager::scanResults() {
  Lock lock(cfgMux_);  // read on the web task while serviceScan() writes on loop
  return cachedNetworks_;
}

void WifiManager::serviceScan() {
  const int n = WiFi.scanComplete();
  if (scanPending_.load()) {
    if (n == kScanFailed) {
      // Failed or timed out (the core cleared its scanning bit). Drop it so the
      // UI stops waiting and a later request can retry, instead of wedging.
      WiFi.scanDelete();
      scanPending_.store(false);
      log_w("wifi: scan failed");
      return;
    }
    if (n >= 0) {  // finished -> cache and free
      // Build off to the side, then swap in under the lock, so a concurrent
      // scanResults() copy never walks a half-mutated vector.
      std::vector<WifiNetwork> fresh;
      fresh.reserve(n);
      for (int i = 0; i < n; ++i) {
        WifiNetwork net;
        net.ssid = WiFi.SSID(i).c_str();
        net.rssi = WiFi.RSSI(i);
        net.secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        if (!net.ssid.empty()) fresh.push_back(net);
      }
      WiFi.scanDelete();
      scanPending_.store(false);
      const size_t count = fresh.size();
      {
        Lock lock(cfgMux_);
        cachedNetworks_.swap(fresh);
      }
      log_i("wifi: cached %u networks", (unsigned)count);
    }
    return;
  }
  if (needScan_.load() && n != kScanRunning) {
    // Mark in flight before clearing the request, so scanRunning() never reads
    // a false "idle" in between.
    scanPending_.store(true);
    needScan_.store(false);
    if (WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false) != kScanRunning) {
      scanPending_.store(false);  // couldn't start; don't wait on a phantom scan
      log_w("wifi: scan could not start");
    }
  }
}

bool WifiManager::tryConnect(const std::string& ssid, const std::string& pass) {
  if (ssid.empty() || ssid.size() > 32 || pass.size() > 63) return false;
  // Runs on the web task: only stage the request. WiFi.mode()/begin() are not
  // reentrant, so the connect itself happens on loop() (applyPendingConnect).
  {
    Lock lock(cfgMux_);
    pendingSsid_ = ssid;
    pendingPass_ = pass;
  }
  pendingConnect_.store(true);
  return true;
}

void WifiManager::applyPendingConnect() {
  if (!pendingConnect_.exchange(false)) return;
  {
    Lock lock(cfgMux_);
    cfg_.ssid = pendingSsid_;
    cfg_.pass = pendingPass_;
  }
  // These creds are unproven: mark dirty so they persist only on got-IP (a typo
  // must not overwrite known-good saved credentials).
  credsDirty_ = true;
  staAttempts_ = 0;
  // Keep the softAP up while onboarding from it, so the phone stays connected.
  // Keyed to the AP actually being up, not mode_ (which reads kConnecting during
  // a portal retry, and would otherwise tear the AP down mid-onboarding).
  startSta(/*keepApDuringConnect=*/apUp_);
}

}  // namespace fogger
