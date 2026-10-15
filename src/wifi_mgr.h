// wifi_mgr.h — Wi-Fi station with captive-portal AP fallback.
//
// On boot: if STA credentials are saved, try them; on failure (or if none are
// saved) bring up a SoftAP + DNS captive portal so the operator can pick a
// network. The web server serves the portal UI and reads status here.
#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <WiFi.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace fogger {

enum class WifiMode : uint8_t { kBooting, kConnecting, kStaConnected, kApPortal };
const char* wifiModeName(WifiMode mode);

struct WifiStatus {
  WifiMode mode = WifiMode::kBooting;
  std::string ssid;  // STA network when connected, else the AP SSID
  std::string ip;    // current IPv4 (STA local IP or AP IP)
  int rssi = 0;      // STA RSSI in dBm (0 while in AP/portal)
  bool portal = false;
};

struct WifiNetwork {
  std::string ssid;
  int rssi = 0;
  bool secure = true;
};

struct WifiConfig {
  std::string ssid;                   // saved STA creds; "" => portal on boot
  std::string pass;
  std::string apName = "Wraith-setup";  // captive-portal SSID; also the auto hostname
  std::string apPass;                 // "" => open AP
  std::string hostname = "wraith";    // mDNS name -> http://<hostname>.local
  uint32_t staConnectTimeoutMs = 15000;
  uint32_t staRetryMs = 60000;        // re-attempt STA from portal, if creds set
};

class WifiManager {
 public:
  bool begin(const WifiConfig& cfg);
  void loop();

  WifiStatus status() const;
  WifiMode mode() const { return mode_.load(); }  // cheap; no lock, no driver call
  bool isPortal() const { return mode_.load() == WifiMode::kApPortal; }
  std::string hostname() const;

  // Invoked once, on the loop task, when an STA connection reaches got-IP. The
  // app uses it to persist credentials only after they are proven to work (so a
  // typo cannot overwrite known-good saved creds).
  using ConnectedCb = std::function<void(const std::string& ssid,
                                         const std::string& pass)>;
  void onStaConnected(ConnectedCb cb) { onConnected_ = std::move(cb); }

  // Scan access. Without force, serves the cached pre-scan (no radio disruption;
  // only the portal auto-scans when its cache is empty). With force=true,
  // requests a fresh scan (blips the softAP client / stalls LAN HTTP ~10 s) —
  // used by the explicit "Scan" button. Poll scanRunning()/scanResults().
  void startScan(bool force = false);
  bool scanRunning() const;
  std::vector<WifiNetwork> scanResults();

  // Stage new STA credentials (any task). The Wi-Fi driver is not reentrant, so
  // the actual connect happens on loop(), the driver's single owner. They are
  // persisted (via onStaConnected) only after got-IP; status() reports progress.
  bool tryConnect(const std::string& ssid, const std::string& pass);

  // Change the mDNS hostname at runtime (any task; re-registered on loop() if
  // connected). "" reverts to the auto name (the device id, cfg.apName).
  void setHostname(const std::string& name);

  // Captive-portal DNS; the web server calls dns()->processNextRequest() too,
  // but loop() already services it.
  DNSServer* dns() { return &dns_; }

 private:
  void startSta(bool keepApDuringConnect = false);
  void startPortal();
  void stopPortal();  // tear down softAP + DNS once STA is up
  void startMdns();  // (re)start mDNS responder once STA is connected
  void serviceScan();  // run a requested scan and cache the results
  void applyPendingConnect();  // consume a tryConnect() on loop()

  WifiConfig cfg_;
  void* cfgMux_ = nullptr;  // guards cfg_ + pending creds + scan cache strings
  std::atomic<WifiMode> mode_{WifiMode::kBooting};

  // Cross-task requests (set on the web task, consumed on loop()).
  std::atomic<bool> pendingConnect_{false};  // pendingSsid_/Pass_ await loop()
  std::atomic<bool> pendingMdns_{false};     // hostname changed; re-register
  std::string pendingSsid_;                  // guarded by cfgMux_
  std::string pendingPass_;

  // Loop-owned state (only ever touched on the loop() task).
  uint32_t staDeadlineMs_ = 0;  // connect attempt timeout
  uint32_t nextRetryMs_ = 0;    // next STA retry while in portal
  uint8_t staAttempts_ = 0;     // consecutive failed STA attempts (backoff)
  bool credsDirty_ = false;     // creds changed via tryConnect, persist on got-IP
  bool apUp_ = false;           // softAP currently running (avoid re-kicking it)
  // Last credentials proven to work; restored into cfg_ if an unproven candidate
  // (from tryConnect) fails, so a typo can't strand the device on a bad network.
  std::string goodSsid_;
  std::string goodPass_;
  ConnectedCb onConnected_;
  DNSServer dns_;

  // Networks are scanned once when the portal comes up (before any client is
  // connected) and cached, because a live scan drops the softAP client. On the
  // LAN (STA) a scan runs only when the UI asks for one.
  std::vector<WifiNetwork> cachedNetworks_;    // guarded by cfgMux_
  std::atomic<bool> needScan_{false};    // request a (re)scan on the next loop
  std::atomic<bool> scanPending_{false};  // a scan is in flight
};

}  // namespace fogger
