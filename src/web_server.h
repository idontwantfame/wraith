// web_server.h — async HTTP + WebSocket control surface.
//
// Serves the SPA from LittleFS, exposes a REST API (/api/*), pushes live status
// over a WebSocket (/ws), hosts the captive-portal redirect, and accepts web
// OTA uploads. Command handlers latch thread-safe intents into the BLE layer.
//
// NOTE: this is an UNAUTHENTICATED LAN control surface by design (small trusted
// network). Do not expose it to the internet; see README "Boundaries".
#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

#include <atomic>

#include "config.h"
#include "fogger_ble.h"
#include "mqtt_ha.h"
#include "ota.h"
#include "sequencer.h"
#include "timekeeper.h"
#include "wifi_mgr.h"

namespace fogger {

struct WebServerDeps {
  FoggerBle* fogger = nullptr;
  WifiManager* wifi = nullptr;
  Config* config = nullptr;
  OtaUpdater* ota = nullptr;
  TimeKeeper* time = nullptr;
  Sequencer* seq = nullptr;
  MqttHa* mqtt = nullptr;  // optional: connection state shown in the UI
};

class WebServer {
 public:
  bool begin(const WebServerDeps& deps);
  void loop();  // WebSocket housekeeping + 1 s status heartbeat (loop task)

  // Broadcast the latest telemetry to all connected WebSocket clients.
  void pushStatus(const FoggerSnapshot& snap);

  // Set true by /api/reboot, OTA success, or factory reset; main acts on it.
  bool rebootPending() const { return rebootPending_; }

  // One-shot: true once after a client requested an IP timezone guess. main
  // performs the (blocking) lookup off the async task.
  bool consumeTzGuessRequest() {
    const bool v = tzGuess_;
    tzGuess_ = false;
    return v;
  }

 private:
  void registerRoutes();
  void handleWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                     AwsEventType type, void* arg, uint8_t* data, size_t len);
  std::string buildStatusJson() const;
  void broadcastStatus();      // push status to all WS clients (loop task)
  uint32_t lastPushMs_ = 0;    // last push (frame-driven or heartbeat); loop task

  // Reject off-LAN / cross-origin requests to state-changing routes (defeats
  // DNS-rebinding and CSRF against this unauthenticated LAN surface). Returns
  // true if the request may proceed; otherwise it has already sent a 403.
  bool guardMutation(AsyncWebServerRequest* req);
  bool hostAllowed(const String& host) const;  // host matches one of ours
  bool requestOnAp(AsyncWebServerRequest* req) const;  // arrived on the softAP iface
  // A foreign-Host request on the softAP (captive probe / rebinding attempt):
  // redirect it to our own address instead of serving it.
  bool needsPortalRedirect(AsyncWebServerRequest* req) const;

  AsyncWebServer server_{80};
  AsyncWebSocket ws_{"/ws"};
  WebServerDeps deps_;
  std::atomic<bool> rebootPending_{false};
  std::atomic<bool> tzGuess_{false};
  // Server-side fog-start arm: single-use, valid for kArmWindowMs after arming.
  // Stored as a flag + a 64-bit microsecond start time (esp_timer, which never
  // wraps), so an expired arm can never come back into validity. Async-task only.
  bool armed_ = false;
  int64_t armStartUs_ = 0;
  bool consumeArm();  // true if armed within the window; always disarms
  // Fog on, a program running, or a STOP still owed to a machine whose link
  // dropped mid-fog (no scans / OTA then: an OTA reboot would discard it).
  bool fogBusy() const;
  AsyncWebServerRequest* otaReq_ = nullptr;  // the one request that owns the OTA session
  uint32_t otaStartMs_ = 0;    // when the owning OTA upload began (stall timeout)
};

}  // namespace fogger
