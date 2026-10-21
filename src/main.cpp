// main.cpp — Wraith controller entry point.
//
// Boot order: Config(NVS) -> BLE central -> Wi-Fi (STA/portal) -> web server
// (the UI is compiled into the firmware) -> MQTT/Home Assistant. The BLE status
// callback fans telemetry out to the web WebSocket and to MQTT. A tiny serial
// fallback keeps a STOP within reach during bench work.
//
// Safety (see docs/ARCHITECTURE.md): never auto-starts fog on boot;
// remote fog START over MQTT is gated by mqtt.allowRemoteIgnition; the web UI
// requires an explicit confirm; STOP is always one action.
#include <Arduino.h>

#include "config.h"
#include "fogger_ble.h"
#include "fw_version.h"
#include "mqtt_ha.h"
#include "ota.h"
#include "sequencer.h"
#include "timekeeper.h"
#include "web_server.h"
#include "wifi_mgr.h"

namespace {

fogger::Config g_config;
fogger::FoggerBle g_fogger;
fogger::WifiManager g_wifi;
fogger::OtaUpdater g_ota;
fogger::TimeKeeper g_time;
fogger::Sequencer g_seq;
fogger::WebServer g_web;
fogger::MqttHa g_mqtt;

String g_line;

// Stable per-device id from the eFuse MAC, e.g. "wraith-7b3c20". getEfuseMac()
// stores the six MAC octets low-byte-first, so the device's last three octets
// (what you see on the label / router) are bytes 3..5 -> shifts 24/32/40.
std::string makeDeviceId() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[20];
  snprintf(buf, sizeof(buf), "wraith-%02x%02x%02x", (unsigned)((mac >> 24) & 0xFF),
           (unsigned)((mac >> 32) & 0xFF), (unsigned)((mac >> 40) & 0xFF));
  return std::string(buf);
}

// Blocking IP-geolocation TZ guess (HTTP GET up to ~8 s). Runs in its own task
// so it never stalls loop() — where BLE STOP writes and the sequencer cap live.
volatile bool g_tzGuessRunning = false;
uint32_t g_lastTzGuessMs = 0;
constexpr uint32_t kTzGuessCooldownMs = 60000;  // rate-limit; unauth-triggerable

// `timeGen` is the config's time generation when the guess was requested: if
// the user saves a timezone during the (slow) lookup, the guess is discarded.
void performTzGuess(uint32_t timeGen) {
  const std::string tz = g_time.guessTimezoneViaIp();
  if (tz.empty()) return;
  if (!g_config.applyGuessedTimezone(tz, timeGen)) {
    Serial.println(F("time: IP guess discarded (timezone changed meanwhile)"));
    return;
  }
  // Apply what Config now holds (not `tz`), so a user save racing this task
  // can't leave the live TZ out of step with the stored one.
  g_time.applyTimezone(g_config.get().timezone);
  Serial.printf("time: guessed timezone %s from IP\n", tz.c_str());
}

void tzGuessTask(void* arg) {
  performTzGuess(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg)));
  g_tzGuessRunning = false;
  vTaskDelete(nullptr);
}

// Kick the guess off-loop, at most one at a time and no more than once per
// cooldown, so a client spamming /api/time/guess can't block the main loop.
void startTzGuess() {
  if (g_tzGuessRunning) return;
  const uint32_t now = millis();
  if (g_lastTzGuessMs != 0 && (now - g_lastTzGuessMs) < kTzGuessCooldownMs) return;
  g_lastTzGuessMs = now;
  g_tzGuessRunning = true;
  void* timeGen = reinterpret_cast<void*>(static_cast<uintptr_t>(g_config.timeGeneration()));
  if (xTaskCreate(tzGuessTask, "tzguess", 8192, timeGen, 1, nullptr) != pdPASS) {
    g_tzGuessRunning = false;  // couldn't spawn; allow a later retry
  }
}

// Auto-guess the timezone once, on first STA connection, if none was chosen.
// (A flag, not "timezone == GMT0": a user who deliberately picks UTC keeps it.)
void maybeAutoGuessTimezone() {
  static bool done = false;
  if (done) return;
  if (g_wifi.mode() != fogger::WifiMode::kStaConnected) return;  // cheap check
  if (g_config.get().timezoneChosen) {
    done = true;
    return;
  }
  done = true;  // one attempt; the UI button can retry
  startTzGuess();
}

// Serial and MQTT power-off (the web route does the same through its deps).
// A running program ends first, so it can't fire again.
bool powerOffMachine() {
  if (g_seq.running()) g_seq.stop("powered off");
  return g_fogger.requestPowerOff();
}

void pollSerial() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      g_line.trim();
      if (g_line == "stop" || g_line == "x") {
        // End any running program first (like the web/MQTT STOP), or its next
        // pulse would re-ignite fog after this STOP. stop() latches STOP under
        // the sequencer lock, so a racing program start can't outlive it.
        g_seq.stop();
        Serial.println(g_fogger.requestFogStop() ? F("-> STOP queued")
                                                 : F("-> rejected (not connected)"));
      } else if (g_line == "status") {
        const auto s = g_fogger.snapshot();
        Serial.printf("link=%s fog=%d batt=%u%% ip=%s\n", fogger::linkStateName(s.link),
                      s.status.isFogging(), s.status.batteryMin(),
                      g_wifi.status().ip.c_str());
      } else if (g_line == "poweroff") {
        Serial.println(powerOffMachine()
                           ? F("-> power-off queued; the machine switches off in a few seconds")
                           : F("-> rejected (not connected)"));
      } else if (g_line.length() > 0) {
        Serial.println(
            F("serial: 'stop'/'x', 'status' or 'poweroff' (full control is in the web UI)"));
      }
      g_line = "";
    } else if (g_line.length() < 32) {
      g_line += c;
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  // Serial is the C3's native USB console. Never let a write wait for a host
  // that is plugged in but not reading: it would stall loop(), and with it STOP.
  Serial.setTxTimeoutMs(0);
#endif
  delay(200);
  Serial.println(F("\nWraith controller booting…"));

  if (!g_config.begin()) {
    Serial.println(F("WARN: config/NVS unavailable; using defaults"));
  }
  // This running build is the new OTA anti-downgrade floor (never lowers).
  // fwBuild() reads the embedded marker, which keeps it in the image. Use the
  // running build as a hard floor even if the NVS bump failed, so a flash error
  // can't leave the floor at 0 and accept an older signed image (fail closed).
  if (!g_config.bumpMinBuild(fogger::fwBuild())) {
    Serial.println(F("WARN: could not persist OTA floor; using running build"));
  }
  const uint32_t stored = g_config.get().minBuild;
  g_ota.setMinBuild(stored > fogger::fwBuild() ? stored : fogger::fwBuild());

  // BLE central. Empty target => discover-only until one is chosen in the UI.
  fogger::FoggerBleConfig bcfg;
  bcfg.targetMac = g_config.get().targetMac;
  // NimBLEDevice::init() brings the radio (and the TRNG) up; the AP password is
  // minted just below, so warn if BLE init failed (RNG may be weakly seeded).
  if (!g_fogger.begin(bcfg)) {
    Serial.println(F("WARN: BLE init failed; AP-password entropy may be reduced"));
  }

  // Mint the per-device AP password now that the radio is initialised (well-
  // seeded RNG), then take the config snapshot the rest of setup uses.
  g_config.ensureApPassword();
  const fogger::ConfigData cfg = g_config.get();

  // Wi-Fi: saved STA creds, else captive portal.
  fogger::WifiConfig wcfg;
  wcfg.ssid = cfg.wifiSsid;
  wcfg.pass = cfg.wifiPass;
  wcfg.apName = makeDeviceId();      // AP SSID stays unique/stable
  wcfg.apPass = cfg.apPassword;      // per-device WPA2 password (never open)
  // mDNS hostname: use the configured name if set, else the auto device id.
  wcfg.hostname = cfg.hostname.empty() ? makeDeviceId() : cfg.hostname;
  // Persist STA creds only after they are proven to work (deferred to got-IP).
  g_wifi.onStaConnected(
      [](const std::string& ssid, const std::string& pass) {
        g_config.setWifi(ssid, pass);
      });
  g_wifi.begin(wcfg);
  Serial.printf("setup AP: SSID '%s'  password '%s'\n", wcfg.apName.c_str(),
                cfg.apPassword.c_str());

  // Wall-clock time via SNTP (syncs once the network is up).
  fogger::TimeConfig tcfg;
  tcfg.tz = cfg.timezone;
  tcfg.ntp1 = cfg.ntpServer;
  g_time.begin(tcfg);

  // Timed programs ("modes") drive the same BLE command path. Wire BEFORE the
  // web server starts, so an early request can't hit unwired callbacks.
  g_seq.wire([] { return g_fogger.requestFogStart(); },
             [] { return g_fogger.requestFogStop(); },
             [](uint32_t d) { return g_fogger.requestDensity(d); },
             [](uint8_t f) { return g_fogger.requestFan(f); },
             [] { return g_fogger.connected(); },
             [] {
               const fogger::FoggerSnapshot s = g_fogger.snapshot();
               return fogger::readOutput(s.connected, s.status.valid,
                                         s.status.outputActive != 0, s.lastStatusMs,
                                         millis());
             });

  // Web control surface.
  fogger::WebServerDeps deps;
  deps.fogger = &g_fogger;
  deps.wifi = &g_wifi;
  deps.config = &g_config;
  deps.ota = &g_ota;
  deps.time = &g_time;
  deps.seq = &g_seq;
  deps.mqtt = &g_mqtt;  // connection state for the UI
  g_web.begin(deps);

  // MQTT / Home Assistant (plain, TLS, or (secure) WebSocket per the settings).
  fogger::MqttHaConfig mcfg;
  mcfg.broker = cfg.mqtt;
  mcfg.deviceId = makeDeviceId();
  mcfg.time = &g_time;
  // MQTT fog commands also cancel any running program so it cannot re-fire.
  // These run on the MQTT task; the sequencer and BLE request API are
  // thread-safe.
  g_mqtt.onCommands(
      [] { g_seq.cancel(); return g_fogger.requestFogStart(); },
      // stop() (not cancel()): ends any program and latches STOP atomically
      // under the sequencer lock, so a web program start racing this MQTT OFF
      // (async task preempting the MQTT task) can't leave a program running.
      [] { g_seq.stop(); return g_fogger.requestFogStop(); },
      [](uint32_t d) { return g_fogger.requestDensity(d); },
      [](uint8_t f) { return g_fogger.requestFan(f); },
      [] { return powerOffMachine(); });
  g_mqtt.begin(mcfg);

  // Fan telemetry out to every consumer from the loop thread.
  g_fogger.onStatus([](const fogger::FoggerSnapshot& snap) {
    g_web.pushStatus(snap);
    g_mqtt.publishState(snap);
  });

  Serial.printf("ready: id=%s build=%u (floor=%u)\n", makeDeviceId().c_str(),
                (unsigned)fogger::fwBuild(), (unsigned)g_config.get().minBuild);
}

void loop() {
  g_fogger.loop();
  g_wifi.loop();
  g_web.loop();
  g_seq.loop();  // (MQTT runs on its own task; see mqtt_ha.h)
  pollSerial();

  maybeAutoGuessTimezone();
  if (g_web.consumeTzGuessRequest()) startTzGuess();

  if (g_web.rebootPending()) {
    // Latch a STOP and pump the BLE loop briefly so the machine isn't left
    // spraying when the link drops on restart (reboot / factory reset / OTA).
    Serial.println(F("stopping fog, then rebooting…"));
    // The MQTT task and the web task keep running during the flush below, so
    // block any START they might latch before queueing the final STOP.
    g_fogger.inhibitStarts();
    g_seq.cancel();
    g_fogger.requestFogStop();
    for (int i = 0; i < 20; ++i) {  // ~400 ms to flush the STOP write
      g_fogger.loop();
      delay(20);
    }
    ESP.restart();
  }
  delay(5);
}
