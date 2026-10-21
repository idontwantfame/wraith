// web_server.cpp — async HTTP + WebSocket control surface. See web_server.h.
#include "web_server.h"

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <esp_timer.h>
#include <mbedtls/x509_crt.h>

#include <cstring>

#include "fw_version.h"
#include "web_assets.h"  // generated from data/ by tools/web_assets.py

namespace fogger {
namespace {

void sendJson(AsyncWebServerRequest* req, int code, const std::string& body) {
  req->send(code, "application/json", body.c_str());
}

// The web UI is compiled into the firmware, gzipped (tools/web_assets.py), so it
// is covered by the image signature and always matches the firmware.
void sendAsset(AsyncWebServerRequest* req, const web::Asset& asset) {
  AsyncWebServerResponse* res =
      req->beginResponse(200, asset.contentType, asset.gzip, asset.gzipLen);
  res->addHeader("Content-Encoding", "gzip");
  res->addHeader("Cache-Control", "no-cache");  // revalidate: it changes with each update
  req->send(res);
}

const web::Asset& indexAsset() {
  for (size_t i = 0; i < web::kAssetCount; ++i) {
    if (std::strcmp(web::kAssets[i].path, "/index.html") == 0) return web::kAssets[i];
  }
  return web::kAssets[0];  // unreachable: the generator requires index.html
}

// Lowercase host, with any :port stripped.
String bareHost(const String& in) {
  String h = in;
  const int colon = h.indexOf(':');
  if (colon >= 0) h = h.substring(0, colon);
  h.toLowerCase();
  return h;
}

// Same-origin check: if an Origin is present it must be exactly
// "http://<our Host>" (scheme and port included; the device only serves HTTP).
bool originMatchesHost(AsyncWebServerRequest* req) {
  if (!req->hasHeader("Origin")) return true;
  String origin = req->header("Origin");
  String host = req->host();
  origin.toLowerCase();
  host.toLowerCase();
  return origin == "http://" + host;
}

void sendResult(AsyncWebServerRequest* req, bool ok, const char* err = nullptr) {
  JsonDocument doc;
  doc["ok"] = ok;
  if (!ok && err) doc["error"] = err;
  std::string out;
  serializeJson(doc, out);
  sendJson(req, ok ? 200 : 400, out);
}

// Largest JSON body a route needs; the MQTT form gets more for a pasted CA
// certificate (up to 3000 chars, JSON-escaped).
constexpr int kMaxJsonBodyBytes = 2048;
constexpr int kMaxMqttBodyBytes = 6144;

// A fog/program START must follow POST /api/fog/arm within this window.
constexpr uint32_t kArmWindowMs = 8000;

// Longest gap between WebSocket status pushes (keeps the UI clock ticking).
constexpr uint32_t kStatusHeartbeatMs = 1000;

// Per-request OTA upload state (stored in req->_tempObject).
constexpr uint8_t kOtaOwner = 1;        // this request owns the OTA session
constexpr uint8_t kOtaRejected = 2;     // refused (guard / busy session / begin)
constexpr uint8_t kOtaNoSignature = 3;  // no ?sig= on an upload that needs one
constexpr uint8_t kOtaFogBusy = 4;      // refused while fog / a program runs

// Whole-upload cap for an OTA image, so a trickled upload can't hold the OTA
// session forever (a silent client is already cut by the server's 3 s rx
// timeout). Generous: with BLE scanning sharing the C3 radio a 1.7 MB image
// was measured at ~55 KB/s (~31 s), and a weaker link takes longer.
constexpr int32_t kOtaMaxUploadMs = 180000;

// JSON routes are POST-only (the library otherwise also answers GET with an
// empty body, running the handler on defaults) and size-capped (bodies are
// buffered before the handler's guardMutation can run).
AsyncCallbackJsonWebHandler* jsonPost(const char* uri, ArJsonRequestHandlerFunction fn,
                                      int maxBodyBytes = kMaxJsonBodyBytes) {
  auto* h = new AsyncCallbackJsonWebHandler(uri, std::move(fn));
  h->setMethod(HTTP_POST);
  h->setMaxContentLength(maxBodyBytes);
  return h;
}

// Constant-time compare (only the length leaks), so response timing can't
// reveal how much of a guessed secret matched.
bool secretEquals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  return diff == 0;
}

// A PEM CA certificate that mbedTLS actually parses (the shape check alone would
// let a garbled paste through, only to fail every TLS handshake later).
bool caPemParses(const std::string& pem) {
  if (!looksLikeCaPem(pem)) return false;
  mbedtls_x509_crt crt;
  mbedtls_x509_crt_init(&crt);
  const int rc = mbedtls_x509_crt_parse(
      &crt, reinterpret_cast<const unsigned char*>(pem.c_str()), pem.size() + 1);
  mbedtls_x509_crt_free(&crt);
  return rc == 0;
}

// Parses + validates the /api/mqtt form into `s` (password and CA are resolved
// by the caller). Returns nullptr on success, else the error to report.
const char* parseMqttForm(JsonVariant& json, MqttSettings& s, std::string& caIn) {
  s.enabled = json["enabled"] | false;
  s.host = json["host"] | "";
  const long port = json["port"] | 1883;
  if (port < 1 || port > 65535) return "invalid port";
  s.port = static_cast<uint16_t>(port);
  s.transport = json["transport"] | "mqtt";
  s.path = json["path"] | "/mqtt";
  s.user = json["user"] | "";
  s.baseTopic = json["base_topic"] | "wraith";
  s.skipHostnameCheck = json["skip_hostname_check"] | false;
  s.allowRemoteIgnition = json["allow_remote_ignition"] | false;
  caIn = json["ca_pem"] | "";
  if (s.host.size() > 253 || s.user.size() > 128 ||
      std::string(json["pass"] | "").size() > 128) {
    return "value too long";
  }
  if (!isValidMqttTransport(s.transport)) return "invalid transport";
  if (!mqttTransportSupported(s.transport)) {
    return "TLS isn't available in this firmware build (not enough RAM on this board)";
  }
  if (!isValidMqttBaseTopic(s.baseTopic)) return "invalid base_topic";
  if (!mqttTransportIsWebSocket(s.transport)) s.path = "/mqtt";  // unused; keep sane
  if (!isValidWebSocketPath(s.path)) return "invalid WebSocket path (e.g. /mqtt)";
  if (!caIn.empty() && !caPemParses(caIn)) {
    return "invalid CA certificate (one PEM CERTIFICATE, max 3000 chars)";
  }
  return nullptr;
}

// Remote ignition lets whoever can publish our command topic start fog from
// anywhere. So enabling it, or changing the broker / topic / TLS trust while it
// is on, needs the per-device setup password (serial / label only, never served
// by the API). The shared arm window is not enough: any LAN client can open it,
// turning brief LAN access into a permanent remote ignition channel.
bool ignitionChangeAuthorized(JsonVariant& json, const MqttSettings& cur,
                              const MqttSettings& next, const std::string& apPassword) {
  if (!next.allowRemoteIgnition) return true;
  if (cur.allowRemoteIgnition && sameIgnitionScope(cur, next)) return true;
  const std::string devicePass = json["device_password"] | "";
  return !apPassword.empty() && secretEquals(devicePass, apPassword);
}

// Keep the stored password when the field is blank, but ONLY for the same broker
// account (host/port/user/transport/path): it is never sent to another broker or
// over a weaker transport. clear_password forces empty.
std::string resolveMqttPassword(JsonVariant& json, const std::string& typed,
                                const MqttSettings& cur, const MqttSettings& next) {
  if (json["clear_password"] | false) return std::string();
  if (!typed.empty()) return typed;
  return sameBrokerAccount(cur, next) ? cur.pass : std::string();
}

}  // namespace

bool WebServer::begin(const WebServerDeps& deps) {
  deps_ = deps;
  if (deps_.fogger == nullptr || deps_.wifi == nullptr || deps_.config == nullptr ||
      deps_.ota == nullptr) {
    log_e("web: missing dependencies");
    return false;
  }

  ws_.onEvent([this](AsyncWebSocket* s, AsyncWebSocketClient* c, AwsEventType t,
                     void* arg, uint8_t* data, size_t len) {
    handleWsEvent(s, c, t, arg, data, len);
  });
  // Reject a cross-site WebSocket upgrade (same host/origin rules as the REST
  // guard) so a foreign page can't hijack the live telemetry stream.
  ws_.setFilter([this](AsyncWebServerRequest* req) {
    if (!hostAllowed(req->host())) return false;
    if (!originMatchesHost(req)) return false;
    return true;
  });
  server_.addHandler(&ws_);

  // Security headers on every response (X-Frame-Options stops clickjacking of
  // the two-tap START, which a <meta> CSP cannot).
  DefaultHeaders::Instance().addHeader("X-Frame-Options", "DENY");
  DefaultHeaders::Instance().addHeader("X-Content-Type-Options", "nosniff");
  DefaultHeaders::Instance().addHeader("Referrer-Policy", "no-referrer");

  registerRoutes();
  server_.begin();
  log_i("web: HTTP server started on :80");
  return true;
}

void WebServer::loop() {
  ws_.cleanupClients();
  // Pushes ride on BLE status frames; with the machine off / out of range none
  // arrive, so the UI clock, link state and program countdown would freeze.
  // Send a heartbeat push whenever none has gone out for a while.
  if (millis() - lastPushMs_ >= kStatusHeartbeatMs) broadcastStatus();
}

std::string WebServer::buildStatusJson() const {
  const FoggerSnapshot snap = deps_.fogger->snapshot();
  const StatusFrame& f = snap.status;
  char fault[8];
  snprintf(fault, sizeof(fault), "0x%04x", f.faultMirror);

  JsonDocument doc;
  JsonObject ble = doc["ble"].to<JsonObject>();
  ble["link"] = linkStateName(snap.link);
  ble["connected"] = snap.connected;
  ble["rssi"] = snap.rssi;
  ble["valid"] = f.valid;
  ble["fogging"] = f.isFogging();
  ble["density"] = f.density;
  ble["fan"] = f.fan;
  ble["batt_a"] = f.batteryA;
  ble["batt_b"] = f.batteryB;
  ble["batt_min"] = f.batteryMin();
  ble["charger"] = f.chargerPresent != 0;
  ble["charge_done"] = f.chargeComplete != 0;
  ble["fault"] = fault;
  ble["frames"] = snap.framesParsed;
  ble["target"] = deps_.fogger->targetMac();

  const WifiStatus w = deps_.wifi->status();
  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["mode"] = wifiModeName(w.mode);
  wifi["ssid"] = w.ssid;
  wifi["ip"] = w.ip;
  wifi["rssi"] = w.rssi;
  wifi["bssid"] = w.bssid;
  wifi["channel"] = w.channel;
  wifi["portal"] = w.portal;
  wifi["host"] = deps_.wifi->hostname();

  const ConfigData cfg = deps_.config->get();
  doc["mqtt_enabled"] = cfg.mqtt.enabled;
  JsonObject mq = doc["mqtt"].to<JsonObject>();
  mq["enabled"] = cfg.mqtt.enabled;
  mq["connected"] = deps_.mqtt != nullptr && deps_.mqtt->connected();
  mq["error"] = deps_.mqtt != nullptr ? deps_.mqtt->lastError() : std::string();
  doc["fw_build"] = fwBuild();

  if (deps_.seq != nullptr) {
    const SeqStatus ss = deps_.seq->status();
    JsonObject prog = doc["program"].to<JsonObject>();
    prog["running"] = ss.running;
    prog["phase"] = seqPhaseName(ss.phase);
    prog["cycle"] = ss.cycle;
    prog["repeats"] = ss.repeats;
    prog["phase_remaining_ms"] = ss.phaseRemainingMs;
    prog["last_stop"] = ss.lastStop;
  }

  JsonObject tm = doc["time"].to<JsonObject>();
  if (deps_.time != nullptr) {
    tm["synced"] = deps_.time->synced();
    tm["local"] = deps_.time->localString();
    tm["utc"] = deps_.time->isoUtc();
    tm["tz"] = cfg.timezone;
  } else {
    tm["synced"] = false;
  }

  std::string out;
  serializeJson(doc, out);
  return out;
}

void WebServer::handleWsEvent(AsyncWebSocket*, AsyncWebSocketClient* client,
                              AwsEventType type, void*, uint8_t*, size_t) {
  if (type == WS_EVT_CONNECT) {
    constexpr size_t kMaxWsClients = 4;  // bound slots against exhaustion
    if (ws_.count() > kMaxWsClients) {
      client->close();
      return;
    }
    client->text(buildStatusJson().c_str());  // prime the new client
  }
}

void WebServer::pushStatus(const FoggerSnapshot&) { broadcastStatus(); }

void WebServer::broadcastStatus() {
  lastPushMs_ = millis();  // frame pushes and heartbeats share one cadence
  if (ws_.count() == 0) return;
  if (!ws_.availableForWriteAll()) return;  // don't queue onto slow clients
  ws_.textAll(buildStatusJson().c_str());
}

bool WebServer::hostAllowed(const String& hostIn) const {
  const String host = bareHost(hostIn);
  if (host.length() == 0 || host == "0.0.0.0") return false;  // never an unset iface
  const std::string hn = deps_.wifi->hostname();
  if (!hn.empty()) {
    String mdns = String(hn.c_str()) + ".local";
    mdns.toLowerCase();
    if (host == mdns) return true;
  }
  const String ap = WiFi.softAPIP().toString();
  const String sta = WiFi.localIP().toString();
  if (ap != "0.0.0.0" && host == ap) return true;
  if (sta != "0.0.0.0" && host == sta) return true;
  return false;
}

bool WebServer::requestOnAp(AsyncWebServerRequest* req) const {
  // True only if this request physically arrived on the softAP interface. Tied
  // to the connection, not to mode_, so it can't be stale (e.g. while mode_ is
  // still kApPortal but STA has silently reconnected to the LAN).
  if (req->client() == nullptr) return false;
  return IPAddress(req->client()->localIP()) == WiFi.softAPIP() &&
         WiFi.softAPIP() != IPAddress(0, 0, 0, 0);
}

bool WebServer::consumeArm() {
  // 64-bit microsecond clock (never wraps): valid only within the window after
  // arming, never again once it has passed.
  const int64_t elapsedUs = esp_timer_get_time() - armStartUs_;
  const bool ok = armed_ && elapsedUs >= 0 &&
                  elapsedUs < static_cast<int64_t>(kArmWindowMs) * 1000;
  armed_ = false;  // single-use, whether or not it was still valid
  return ok;
}

bool WebServer::fogBusy() const {
  return deps_.fogger->snapshot().status.isFogging() ||
         deps_.fogger->stopOwed() ||
         (deps_.seq != nullptr && deps_.seq->running());
}

bool WebServer::needsPortalRedirect(AsyncWebServerRequest* req) const {
  // The setup AP's wildcard DNS resolves EVERY name to us, so a page already open
  // on the phone could rebind its own origin onto the device. Instead of letting
  // foreign Hosts through on the AP, bounce them (OS captive probes included) to
  // our own address: the UI then runs with a Host we trust, and no API route
  // ever accepts a foreign Host. A redirect still makes iOS/Android/Windows flag
  // the captive network and open the sign-in sheet.
  return requestOnAp(req) && !hostAllowed(req->host());
}

bool WebServer::guardMutation(AsyncWebServerRequest* req) {
  // Every request must carry one of our Hosts (IP / <hostname>.local), on the
  // LAN and on the softAP alike, which defeats DNS rebinding. Captive-portal
  // probes with foreign Hosts are redirected before they reach the UI.
  if (!hostAllowed(req->host())) {
    sendJson(req, 403, "{\"ok\":false,\"error\":\"bad host\"}");
    return false;
  }
  // A custom header can't be set by a cross-site <form> or simple fetch, so it
  // forces a CORS preflight that this server never answers.
  if (!req->hasHeader("X-Requested-With")) {
    sendJson(req, 403, "{\"ok\":false,\"error\":\"missing X-Requested-With\"}");
    return false;
  }
  // If an Origin is present it must be same-origin with the request's own Host.
  if (!originMatchesHost(req)) {
    sendJson(req, 403, "{\"ok\":false,\"error\":\"bad origin\"}");
    return false;
  }
  return true;
}

void WebServer::registerRoutes() {
  // --- Telemetry ---
  server_.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* req) {
    if (!guardMutation(req)) return;  // discloses MAC/SSID/IP; block rebinding reads
    sendJson(req, 200, buildStatusJson());
  });

  // --- Fog control (no body). Manual control cancels any running program. ---
  // START is a server-enforced two-step: the client must POST /api/fog/arm
  // within kArmWindowMs first, so a single stray request can never ignite fog.
  server_.on("/api/fog/arm", HTTP_POST, [this](AsyncWebServerRequest* req) {
    if (!guardMutation(req)) return;
    armed_ = true;
    armStartUs_ = esp_timer_get_time();
    sendResult(req, true);
  });
  server_.on("/api/fog/start", HTTP_POST, [this](AsyncWebServerRequest* req) {
    if (!guardMutation(req)) return;
    if (!consumeArm()) {
      sendResult(req, false, "not armed");
      return;
    }
    if (deps_.seq != nullptr) deps_.seq->cancel();
    sendResult(req, deps_.fogger->requestFogStart(), "not connected");
  });
  server_.on("/api/fog/stop", HTTP_POST, [this](AsyncWebServerRequest* req) {
    if (!guardMutation(req)) return;  // STOP stays always-available to our UI
    // stop() (not cancel()): ends any program and latches STOP atomically under
    // the sequencer lock, so a racing program start can't outlive this STOP.
    if (deps_.seq != nullptr) deps_.seq->stop();
    sendResult(req, deps_.fogger->requestFogStop(), "not connected");
  });

  // --- Timed programs ("modes") ---
  auto* progStart = jsonPost(
      "/api/program/start", [this](AsyncWebServerRequest* req, JsonVariant& json) {
        if (!guardMutation(req)) return;
        // A program ignites fog, so it needs the same single-use arm as fog/start.
        if (!consumeArm()) {
          sendResult(req, false, "not armed");
          return;
        }
        if (deps_.seq == nullptr) {
          sendResult(req, false, "no sequencer");
          return;
        }
        // Range-check in wide types before narrowing (the Sequencer re-clamps).
        double on = json["on_sec"] | 5.0;
        double off = json["off_sec"] | 5.0;
        long rep = json["repeats"] | 5;
        long fan = json["fan"] | 0;
        on = on < 0 ? 0 : (on > 3600 ? 3600 : on);
        off = off < 0 ? 0 : (off > 3600 ? 3600 : off);
        rep = rep < 0 ? 0 : (rep > 65535 ? 65535 : rep);
        fan = fan < 0 ? 0 : (fan > 255 ? 255 : fan);
        FogProgram p;
        p.onMs = static_cast<uint32_t>(on * 1000);
        p.offMs = static_cast<uint32_t>(off * 1000);
        p.repeats = static_cast<uint16_t>(rep);
        p.density = json["density"] | 0U;
        p.fan = static_cast<uint8_t>(fan);
        sendResult(req, deps_.seq->start(p), "not connected");
      });
  server_.addHandler(progStart);

  server_.on("/api/program/stop", HTTP_POST, [this](AsyncWebServerRequest* req) {
    if (!guardMutation(req)) return;
    if (deps_.seq != nullptr) deps_.seq->stop();
    sendResult(req, true);
  });

  // --- Density / fan (JSON {value}). Builders clamp to the verified ranges. ---
  auto* density = jsonPost(
      "/api/density", [this](AsyncWebServerRequest* req, JsonVariant& json) {
        if (!guardMutation(req)) return;
        JsonVariant v = json["value"];
        if (!v.is<double>()) {
          sendResult(req, false, "numeric value required");
          return;
        }
        double dv = v.as<double>();
        dv = dv < 0 ? 0 : (dv > 100000 ? 100000 : dv);
        sendResult(req, deps_.fogger->requestDensity(static_cast<uint32_t>(dv)),
                   "not connected");
      });
  server_.addHandler(density);

  auto* fan = jsonPost(
      "/api/fan", [this](AsyncWebServerRequest* req, JsonVariant& json) {
        if (!guardMutation(req)) return;
        JsonVariant v = json["value"];
        if (!v.is<double>()) {
          sendResult(req, false, "numeric value required");
          return;
        }
        double dv = v.as<double>();
        dv = dv < 0 ? 0 : (dv > 255 ? 255 : dv);
        sendResult(req, deps_.fogger->requestFan(static_cast<uint8_t>(dv)),
                   "not connected");
      });
  server_.addHandler(fan);

  // --- Machine discovery + selection ---
  server_.on("/api/machines", HTTP_GET, [this](AsyncWebServerRequest* req) {
    if (!guardMutation(req)) return;  // discloses MACs; block rebinding reads
    JsonDocument doc;
    doc["target"] = deps_.fogger->targetMac();
    JsonArray arr = doc["machines"].to<JsonArray>();
    for (const auto& m : deps_.fogger->discoveredMachines()) {
      JsonObject o = arr.add<JsonObject>();
      o["mac"] = m.mac;
      o["name"] = m.name;
      o["rssi"] = m.rssi;
    }
    std::string out;
    serializeJson(doc, out);
    sendJson(req, 200, out);
  });

  auto* selectMachine = jsonPost(
      "/api/machine/select", [this](AsyncWebServerRequest* req, JsonVariant& json) {
        if (!guardMutation(req)) return;
        const std::string mac = (json["mac"] | "");
        if (!deps_.config->setTargetMac(mac)) {
          sendResult(req, false, "invalid MAC");
          return;
        }
        // A program must not keep pulsing across a (re)selection. stop(), not
        // cancel(): it latches a STOP, so fog ends even when the same connected
        // machine is re-selected (a no-op in the BLE layer). On a switch the BLE
        // layer also writes STOP to the old machine before dropping its link.
        if (deps_.seq != nullptr && deps_.seq->running()) deps_.seq->stop();
        deps_.fogger->setTarget(mac);
        sendResult(req, true);
      });
  server_.addHandler(selectMachine);

  server_.on("/api/machine/disconnect", HTTP_POST, [this](AsyncWebServerRequest* req) {
    if (!guardMutation(req)) return;
    // End a running program with a latched STOP (the BLE layer also STOPs the
    // machine before dropping the link).
    if (deps_.seq != nullptr && deps_.seq->running()) deps_.seq->stop();
    deps_.config->setTargetMac("");  // clear so it won't auto-reconnect on boot
    deps_.fogger->setTarget("");     // disconnect + go back to discover-only
    sendResult(req, true);
  });

  // One-way: afterwards only the machine's own button powers it on, so the UI
  // confirms first. A running program ends first so it can't fire again.
  server_.on("/api/machine/power_off", HTTP_POST, [this](AsyncWebServerRequest* req) {
    if (!guardMutation(req)) return;
    if (!deps_.fogger->connected()) {  // nothing to switch off; no "powered off"
      sendResult(req, false, "not connected");
      return;
    }
    if (deps_.seq != nullptr && deps_.seq->running()) deps_.seq->stop("powered off");
    sendResult(req, deps_.fogger->requestPowerOff(), "not connected");
  });

  // --- Wi-Fi scan + connect ---
  server_.on("/api/wifi/scan", HTTP_GET, [this](AsyncWebServerRequest* req) {
    if (!guardMutation(req)) return;  // ?force=1 is state-changing; also a read guard
    // A Wi-Fi scan hops channels on the radio BLE shares: never start one while
    // fog is running (or a program may fire), so it can't disturb the link a
    // STOP depends on. The cached list is still returned.
    const bool busy = fogBusy();
    if (!busy) deps_.wifi->startScan(req->hasParam("force"));  // Scan sends ?force=1
    JsonDocument doc;
    doc["scanning"] = !busy && deps_.wifi->scanRunning();
    doc["busy"] = busy;
    JsonArray arr = doc["networks"].to<JsonArray>();
    for (const auto& n : deps_.wifi->scanResults()) {
      JsonObject o = arr.add<JsonObject>();
      o["ssid"] = n.ssid;
      o["rssi"] = n.rssi;
      o["secure"] = n.secure;
    }
    std::string out;
    serializeJson(doc, out);
    sendJson(req, 200, out);
  });

  auto* wifiConnect = jsonPost(
      "/api/wifi/connect", [this](AsyncWebServerRequest* req, JsonVariant& json) {
        if (!guardMutation(req)) return;
        const std::string ssid = (json["ssid"] | "");
        const std::string pass = (json["pass"] | "");
        // Do NOT persist here: tryConnect() stages the creds and the Wi-Fi
        // manager saves them only after got-IP, so a typo can't clobber the
        // known-good saved network.
        sendResult(req, deps_.wifi->tryConnect(ssid, pass), "invalid credentials");
      });
  server_.addHandler(wifiConnect);

  // --- Config + MQTT/HA ---
  server_.on("/api/config", HTTP_GET, [this](AsyncWebServerRequest* req) {
    if (!guardMutation(req)) return;  // discloses SSID/MQTT host/user; block reads
    const ConfigData c = deps_.config->get();
    JsonDocument doc;
    doc["target_mac"] = c.targetMac;
    doc["wifi_ssid"] = c.wifiSsid;
    doc["timezone"] = c.timezone;
    doc["ntp_server"] = c.ntpServer;
    doc["hostname"] = c.hostname;
    JsonObject mqtt = doc["mqtt"].to<JsonObject>();
    mqtt["enabled"] = c.mqtt.enabled;
    mqtt["host"] = c.mqtt.host;
    mqtt["port"] = c.mqtt.port;
    mqtt["transport"] = c.mqtt.transport;
    mqtt["path"] = c.mqtt.path;
    mqtt["user"] = c.mqtt.user;
    mqtt["base_topic"] = c.mqtt.baseTopic;
    mqtt["allow_remote_ignition"] = c.mqtt.allowRemoteIgnition;
    mqtt["has_password"] = !c.mqtt.pass.empty();  // never echo the secret
    mqtt["has_ca"] = !c.mqtt.caPem.empty();
    mqtt["skip_hostname_check"] = c.mqtt.skipHostnameCheck;
    mqtt["tls_supported"] = kMqttTlsSupported;  // build capability (WRAITH_MQTT_TLS)
    std::string out;
    serializeJson(doc, out);
    sendJson(req, 200, out);
  });

  auto* mqtt = jsonPost(
      "/api/mqtt", [this](AsyncWebServerRequest* req, JsonVariant& json) {
        if (!guardMutation(req)) return;
        MqttSettings next;
        std::string caIn;
        if (const char* err = parseMqttForm(json, next, caIn)) {
          sendResult(req, false, err);
          return;
        }
        const ConfigData cur = deps_.config->get();
        // CA: a pasted one replaces, clear_ca removes, blank keeps the stored one.
        next.caPem = (json["clear_ca"] | false) ? std::string()
                                                : (caIn.empty() ? cur.mqtt.caPem : caIn);
        if (next.enabled && mqttTransportIsTls(next.transport) && next.caPem.empty()) {
          sendResult(req, false, "TLS needs the broker's CA certificate");
          return;
        }
        if (!ignitionChangeAuthorized(json, cur.mqtt, next, cur.apPassword)) {
          sendResult(req, false, "device password required to allow remote START");
          return;
        }
        const std::string typed = (json["pass"] | "");
        next.pass = resolveMqttPassword(json, typed, cur.mqtt, next);
        // Dropping a stored password silently looks like "the save lost my
        // password", so say so (the UI asks the user to re-enter it).
        const bool passwordCleared = typed.empty() && !cur.mqtt.pass.empty() &&
                                     next.pass.empty();
        if (!deps_.config->setMqtt(next)) {
          sendResult(req, false, "invalid MQTT settings");
          return;
        }
        JsonDocument doc;
        doc["ok"] = true;
        if (passwordCleared) doc["password_cleared"] = true;
        std::string out;
        serializeJson(doc, out);
        sendJson(req, 200, out);
        // MQTT settings apply at boot, so reboot server-side right away: revoking
        // remote ignition must not depend on the client sending a reboot.
        rebootPending_ = true;
      },
      kMaxMqttBodyBytes);  // room for a pasted CA certificate
  server_.addHandler(mqtt);

  // --- Time / NTP (JSON {timezone, ntp_server}) ---
  // Registered before /api/time, whose handler also prefix-matches /api/time/*.
  server_.on("/api/time/guess", HTTP_POST, [this](AsyncWebServerRequest* req) {
    if (!guardMutation(req)) return;
    tzGuess_ = true;  // performed off the async task by main loop()
    sendResult(req, deps_.time != nullptr, "no time source");
  });

  auto* timeCfg = jsonPost(
      "/api/time", [this](AsyncWebServerRequest* req, JsonVariant& json) {
        if (!guardMutation(req)) return;
        const std::string tz = (json["timezone"] | "GMT0");
        const std::string ntp = (json["ntp_server"] | "pool.ntp.org");
        if (tz.size() > 63 || ntp.size() > 253) {
          sendResult(req, false, "value too long");
          return;
        }
        const bool ok = deps_.config->setTime(tz, ntp);
        if (ok && deps_.time != nullptr) deps_.time->applyTimezone(tz);
        // A new NTP server takes effect on the next reboot; the TZ is live.
        sendResult(req, ok, "invalid time settings");
      });
  server_.addHandler(timeCfg);

  auto* hostnameCfg = jsonPost(
      "/api/hostname", [this](AsyncWebServerRequest* req, JsonVariant& json) {
        if (!guardMutation(req)) return;
        const std::string h = (json["hostname"] | "");
        if (!deps_.config->setHostname(h)) {
          sendResult(req, false, "invalid hostname");
          return;
        }
        // Live re-register ("" -> the auto name). Report the effective name: a
        // page open on the OLD <name>.local fails the Host check from now on, so
        // the UI must move to the new one.
        deps_.wifi->setHostname(deps_.config->get().hostname);
        JsonDocument doc;
        doc["ok"] = true;
        doc["hostname"] = deps_.wifi->hostname();
        std::string out;
        serializeJson(doc, out);
        sendJson(req, 200, out);
      });
  server_.addHandler(hostnameCfg);

  // --- Lifecycle ---
  server_.on("/api/reboot", HTTP_POST, [this](AsyncWebServerRequest* req) {
    if (!guardMutation(req)) return;
    rebootPending_ = true;
    sendResult(req, true);
  });
  server_.on("/api/factory_reset", HTTP_POST, [this](AsyncWebServerRequest* req) {
    if (!guardMutation(req)) return;
    const bool ok = deps_.config->factoryReset();
    if (ok) rebootPending_ = true;
    sendResult(req, ok, "reset failed");
  });

  // --- OTA upload (multipart) ---
  server_.on(
      "/api/ota", HTTP_POST,
      [this](AsyncWebServerRequest* req) {
        // Only the request that actually owned the OTA session reports a result;
        // a rejected or non-owning upload (second part / concurrent) gets 409.
        const uint8_t* st = static_cast<const uint8_t*>(req->_tempObject);
        const uint8_t state = (st == nullptr) ? kOtaRejected : *st;
        if (state == kOtaNoSignature) {
          sendJson(req, 400, "{\"ok\":false,\"error\":\"signature required (upload the .sig too)\"}");
          return;
        }
        if (state == kOtaFogBusy) {
          sendJson(req, 409,
                   "{\"ok\":false,\"error\":\"fog is running or a STOP is still pending; "
                   "stop the fog / reconnect the machine before updating\"}");
          return;
        }
        if (state != kOtaOwner) {
          sendJson(req, 409,
                   "{\"ok\":false,\"error\":\"upload rejected or another update in progress\"}");
          return;
        }
        const bool ok = deps_.ota->lastSucceeded();
        if (ok) rebootPending_ = true;
        sendResult(req, ok, deps_.ota->lastError());
      },
      [this](AsyncWebServerRequest* req, const String&, size_t index,
             uint8_t* data, size_t len, bool final) {
        if (index == 0) {
          // Per-request state byte (kOta*). Freed by the server with the request.
          if (req->_tempObject == nullptr) {
            uint8_t* st = static_cast<uint8_t*>(malloc(1));
            if (st != nullptr) *st = kOtaRejected;  // default: rejected
            req->_tempObject = st;
          }
          uint8_t* st = static_cast<uint8_t*>(req->_tempObject);
          if (st == nullptr) return;       // OOM: fail closed, never begin
          if (*st == kOtaOwner) return;    // a later part of an upload that already ran
          if (otaReq_ != nullptr) return;  // another upload already owns the session
          // Same host + custom-header + same-origin checks as guardMutation (the
          // upload callback can't send a response itself; completion returns it).
          if (!hostAllowed(req->host())) return;
          if (!req->hasHeader("X-Requested-With")) return;
          if (!originMatchesHost(req)) return;
          // Before any flash erase: an upload with no signature would only be
          // refused at the end, so don't let it erase the slot or hold the OTA
          // session; and no OTA while fog runs (flash erases stall the CPU, and
          // the post-OTA reboot would stop the fog anyway).
          if (deps_.ota->signatureRequired() && !req->hasParam("sig")) {
            *st = kOtaNoSignature;
            return;
          }
          if (fogBusy()) {
            *st = kOtaFogBusy;
            return;
          }
          deps_.ota->abort();              // clear any abandoned session
          if (!deps_.ota->begin(0)) return;  // committed / active / begin failure
          otaReq_ = req;
          otaStartMs_ = millis();
          *st = kOtaOwner;                 // this request owns the session
          req->onDisconnect([this, req] {
            if (otaReq_ == req) {
              deps_.ota->abort();
              otaReq_ = nullptr;
            }
          });
        }
        if (otaReq_ != req) return;  // not the owning upload: ignore these bytes
        // Abort a trickled upload so it can't wedge the OTA session.
        if ((int32_t)(millis() - otaStartMs_) > kOtaMaxUploadMs) {
          deps_.ota->abort("upload took too long");
          otaReq_ = nullptr;
          return;
        }
        deps_.ota->write(data, len);  // a short write aborts internally; end()
                                      // then reports the failure honestly
        if (final) {
          // Detached base64 signature travels as the ?sig= query parameter.
          if (req->hasParam("sig")) {
            deps_.ota->setSignatureB64(req->getParam("sig")->value().c_str());
          }
          // Set the reboot flag here (not only in the completion handler), so a
          // client that drops after the final chunk still reboots into the
          // committed image instead of leaving it staged behind the latch.
          if (deps_.ota->end()) rebootPending_ = true;
          otaReq_ = nullptr;  // session done; committed_ latch blocks further OTA
        }
      });

  // --- Web UI (built into the firmware) + captive portal ---
  // Foreign-Host requests on the softAP skip these routes and fall through to
  // onNotFound, which redirects them to our own address.
  const auto notPortalRedirect = [this](AsyncWebServerRequest* req) {
    return !needsPortalRedirect(req);
  };
  for (size_t i = 0; i < web::kAssetCount; ++i) {
    const web::Asset* asset = &web::kAssets[i];
    server_.on(asset->path, HTTP_GET, [asset](AsyncWebServerRequest* req) {
      sendAsset(req, *asset);
    }).setFilter(notPortalRedirect);
  }
  server_.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    sendAsset(req, indexAsset());
  }).setFilter(notPortalRedirect);
  server_.onNotFound([this](AsyncWebServerRequest* req) {
    // Unknown API paths are real 404s, not the SPA (don't mask typos/errors).
    if (req->url().startsWith("/api/")) {
      sendJson(req, 404, "{\"ok\":false,\"error\":\"not found\"}");
      return;
    }
    // Captive probes (/generate_204, /hotspot-detect.html, /ncsi.txt, …) and any
    // other foreign-Host request on the softAP: redirect to our own address. The
    // non-"success" answer makes the OS open its sign-in sheet on our UI.
    if (needsPortalRedirect(req)) {
      req->redirect(("http://" + WiFi.softAPIP().toString() + "/").c_str());
      return;
    }
    // Serve the UI (HTTP 200) for any other unknown path: the SPA client-routing
    // fallback.
    sendAsset(req, indexAsset());
  });
}

}  // namespace fogger
