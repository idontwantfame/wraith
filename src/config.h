// config.h — persistent controller settings backed by NVS (Preferences).
//
// Single owner of everything that must survive reboot: the bound machine MAC,
// the saved Wi-Fi station credentials, and the MQTT / Home Assistant settings.
// A cached copy is kept so hot readers (web handlers) never touch flash.
#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include <atomic>
#include <string>

#include "mqtt_settings.h"

namespace fogger {

// Full persisted configuration. Values are the in-RAM view of NVS.
struct ConfigData {
  std::string targetMac;        // bound machine BLE MAC; "" = none chosen yet

  std::string wifiSsid;         // saved STA network; "" = none (portal on boot)
  std::string wifiPass;

  // MQTT / Home Assistant broker settings. mqtt.allowRemoteIgnition is the
  // safety gate for fog START (and density/fan) over MQTT: off by default so an
  // automation cannot ignite fog unattended. STOP is always accepted.
  MqttSettings mqtt;

  std::string timezone = "GMT0";          // POSIX TZ string (default UTC)
  std::string ntpServer = "pool.ntp.org";
  // A timezone was chosen (by the user, or a past IP guess). Stops the one-shot
  // boot auto-guess, so a deliberate GMT0 (UTC) is never replaced.
  bool timezoneChosen = false;

  std::string hostname;                    // mDNS/AP name; "" => auto (wraith-xxxxxx)

  // Per-device captive-portal AP password (WPA2). Generated once on first boot
  // and persisted, so the onboarding AP is never open. Printed to serial.
  std::string apPassword;

  // Highest firmware build ever run on this device (OTA anti-downgrade floor).
  uint32_t minBuild = 0;
};

class Config {
 public:
  // Open NVS and populate the cache. Returns false if NVS cannot be opened.
  bool begin();

  // Cached snapshot; cheap, no flash access. Thread-safe (copied under lock).
  ConfigData get() const;

  // Mutators validate, persist to NVS, and update the cache. Return false on
  // invalid input or a flash write failure (cache is left unchanged then).
  // They may be called from any task: one writer lock serialises each whole
  // NVS-write + cache-update, so concurrent setters can't interleave keys.
  bool setTargetMac(const std::string& mac);
  bool setWifi(const std::string& ssid, const std::string& pass);
  bool clearWifi();
  // Validates (transport, port, path, topic, CA required for TLS) and persists.
  bool setMqtt(const MqttSettings& mqtt);
  // User-chosen time settings (marks the timezone as chosen).
  bool setTime(const std::string& timezone, const std::string& ntpServer);
  // Generation of the stored time settings; bumped by every setTime().
  uint32_t timeGeneration() const;
  // Store a timezone guessed from IP (NTP server unchanged), unless setTime()
  // ran since `expectedGeneration` was read: a slow lookup must not overwrite a
  // timezone the user saved meanwhile. False if discarded or on a flash error.
  bool applyGuessedTimezone(const std::string& timezone, uint32_t expectedGeneration);
  bool setHostname(const std::string& hostname);  // "" clears (back to auto)

  // Raise the OTA anti-downgrade floor to at least `build` (monotonic; never
  // lowers it). Called at boot with the running build. Returns false on a flash
  // write failure.
  bool bumpMinBuild(uint32_t build);

  // Mint + persist the per-device AP password if none is stored yet. Call once
  // at boot AFTER the radio is initialised, so the RNG is well seeded.
  void ensureApPassword();

  // Wipe all stored settings (factory reset). Cache resets to defaults.
  bool factoryReset();

  // True for a canonical "AA:BB:CC:DD:EE:FF" MAC. Also accepts "" (= unset).
  static bool isValidMac(const std::string& mac);

 private:
  void loadCache();

  Preferences prefs_;
  ConfigData cache_;
  bool open_ = false;
  void* mutex_ = nullptr;     // guards cache_ (web task vs loop()); SemaphoreHandle_t
  void* writeMux_ = nullptr;  // recursive; serialises writers (NVS + cache)
  // Bumped by setTime() under writeMux_; read lock-free (loop() must never wait
  // on a writer doing a slow NVS commit).
  std::atomic<uint32_t> timeGen_{0};
};

}  // namespace fogger
