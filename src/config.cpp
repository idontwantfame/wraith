// config.cpp — NVS-backed settings. See config.h.
#include "config.h"

#include <cctype>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace fogger {
namespace {

// NVS namespace + keys (keys must be <= 15 chars). The namespace keeps the
// project's original name: renaming it would orphan every saved setting.
constexpr char kNamespace[] = "ulface";
constexpr char kKeyMac[] = "mac";
constexpr char kKeySsid[] = "ssid";
constexpr char kKeyPass[] = "pass";
constexpr char kKeyMqttEn[] = "mqtt_en";
constexpr char kKeyMqttHost[] = "mqtt_host";
constexpr char kKeyMqttPort[] = "mqtt_port";
constexpr char kKeyMqttUser[] = "mqtt_user";
constexpr char kKeyMqttPass[] = "mqtt_pass";
constexpr char kKeyMqttTopic[] = "mqtt_topic";
constexpr char kKeyMqttTransport[] = "mqtt_tr";
constexpr char kKeyMqttPath[] = "mqtt_path";
constexpr char kKeyMqttCa[] = "mqtt_ca";
constexpr char kKeyMqttNoCn[] = "mqtt_nocn";
constexpr char kKeyAllowIgn[] = "allow_ign";
constexpr char kKeyTz[] = "tz";
constexpr char kKeyTzChosen[] = "tz_set";
constexpr char kKeyNtp[] = "ntp";
constexpr char kKeyHost[] = "host";
constexpr char kKeyDevPw[] = "dev_pw";
// Retired: the per-device setup-AP password (the setup network is open now).
// Removed from NVS at boot so the old secret doesn't linger.
constexpr char kKeyOldApPw[] = "ap_pw";
constexpr char kKeyMinBuild[] = "min_build";

constexpr char kDefaultTz[] = "GMT0";
constexpr size_t kMaxSsid = 32;
constexpr size_t kMaxPass = 63;
constexpr size_t kMaxDevicePw = 64;

bool isHexDigit(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

SemaphoreHandle_t asMutex(void* h) { return static_cast<SemaphoreHandle_t>(h); }

// RAII scoped lock over the cache mutex (no-op before begin() creates it).
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

// RAII scoped lock over the recursive writer mutex (factoryReset() and
// clearWifi() re-enter other mutators). Lock order: writer, then cache.
class WriteLock {
 public:
  explicit WriteLock(void* m) : m_(m) {
    if (m_) xSemaphoreTakeRecursive(asMutex(m_), portMAX_DELAY);
  }
  ~WriteLock() {
    if (m_) xSemaphoreGiveRecursive(asMutex(m_));
  }
  WriteLock(const WriteLock&) = delete;
  WriteLock& operator=(const WriteLock&) = delete;

 private:
  void* m_;
};

// MQTT settings as a whole (defence in depth: the web layer validates too). The
// base topic is spliced into publish/subscribe filters, so it is re-checked here
// to keep any future caller from reintroducing a wildcard subscription.
bool mqttSettingsValid(const MqttSettings& m) {
  if (m.enabled && m.host.empty()) return false;  // a broker is required
  if (m.port == 0 || !mqttTransportSupported(m.transport)) return false;
  if (!isValidMqttBaseTopic(m.baseTopic)) return false;
  if (!isValidWebSocketPath(m.path)) return false;  // also bounds it for NVS
  if (!m.caPem.empty() && !looksLikeCaPem(m.caPem)) return false;
  // TLS without a CA would trust nothing (no public CA bundle is built in).
  if (m.enabled && mqttTransportIsTls(m.transport) && m.caPem.empty()) return false;
  return true;
}

}  // namespace

bool Config::isValidMac(const std::string& mac) {
  if (mac.empty()) return true;  // "" means unset
  if (mac.size() != 17) return false;
  for (size_t i = 0; i < mac.size(); ++i) {
    if ((i % 3) == 2) {
      if (mac[i] != ':') return false;
    } else if (!isHexDigit(mac[i])) {
      return false;
    }
  }
  return true;
}

bool Config::begin() {
  if (mutex_ == nullptr) mutex_ = xSemaphoreCreateMutex();
  if (writeMux_ == nullptr) writeMux_ = xSemaphoreCreateRecursiveMutex();
  open_ = prefs_.begin(kNamespace, /*readOnly=*/false);
  if (!open_) {
    log_e("config: failed to open NVS namespace '%s'", kNamespace);
    return false;
  }
  loadCache();
  if (prefs_.isKey(kKeyOldApPw)) prefs_.remove(kKeyOldApPw);  // see kKeyOldApPw
  return true;
}

ConfigData Config::get() const {
  Lock lock(mutex_);
  return cache_;
}

bool Config::mqttEnabled() const {
  Lock lock(mutex_);
  return cache_.mqtt.enabled;
}

std::string Config::timezone() const {
  Lock lock(mutex_);
  return cache_.timezone;
}

bool Config::setDevicePassword(const std::string& password) {
  if (!open_ || password.size() > kMaxDevicePw) return false;
  WriteLock wlock(writeMux_);
  // putString returns 0 for an empty value (a valid "clear").
  if (prefs_.putString(kKeyDevPw, password.c_str()) == 0 && !password.empty()) return false;
  Lock lock(mutex_);
  cache_.devicePassword = password;
  return true;
}

void Config::loadCache() {
  // isKey() guards avoid the Preferences library logging an error for every
  // missing key on first boot (before anything has been saved).
  auto str = [&](const char* k, const char* def) {
    return prefs_.isKey(k) ? std::string(prefs_.getString(k).c_str()) : std::string(def);
  };
  auto boolean = [&](const char* k, bool def) {
    return prefs_.isKey(k) ? prefs_.getBool(k) : def;
  };

  ConfigData d;
  d.targetMac = str(kKeyMac, "");
  d.wifiSsid = str(kKeySsid, "");
  d.wifiPass = str(kKeyPass, "");
  d.mqtt.enabled = boolean(kKeyMqttEn, false);
  d.mqtt.host = str(kKeyMqttHost, "");
  d.mqtt.port = prefs_.isKey(kKeyMqttPort) ? prefs_.getUShort(kKeyMqttPort) : 1883;
  d.mqtt.transport = str(kKeyMqttTransport, "mqtt");  // pre-TLS saves: plain MQTT
  d.mqtt.path = str(kKeyMqttPath, "/mqtt");
  d.mqtt.user = str(kKeyMqttUser, "");
  d.mqtt.pass = str(kKeyMqttPass, "");
  d.mqtt.baseTopic = str(kKeyMqttTopic, "wraith");
  d.mqtt.caPem = str(kKeyMqttCa, "");
  d.mqtt.skipHostnameCheck = boolean(kKeyMqttNoCn, false);
  d.mqtt.allowRemoteIgnition = boolean(kKeyAllowIgn, false);
  d.timezone = str(kKeyTz, kDefaultTz);
  // Settings saved before the tz_set key existed: a non-default timezone means
  // one was already chosen (the old "GMT0 = unset" rule), so no surprise guess.
  d.timezoneChosen = boolean(kKeyTzChosen, d.timezone != kDefaultTz);
  d.ntpServer = str(kKeyNtp, "pool.ntp.org");
  d.hostname = str(kKeyHost, "");
  d.devicePassword = str(kKeyDevPw, "");
  d.minBuild = prefs_.isKey(kKeyMinBuild) ? prefs_.getUInt(kKeyMinBuild) : 0;
  Lock lock(mutex_);
  cache_ = d;
}

bool Config::bumpMinBuild(uint32_t build) {
  if (!open_) return false;
  WriteLock wlock(writeMux_);
  {
    Lock lock(mutex_);
    if (build <= cache_.minBuild) return true;  // never lowers; nothing to do
  }
  if (prefs_.putUInt(kKeyMinBuild, build) == 0) return false;
  Lock lock(mutex_);
  cache_.minBuild = build;
  return true;
}

bool Config::setTargetMac(const std::string& mac) {
  if (!open_ || !isValidMac(mac)) return false;
  WriteLock wlock(writeMux_);
  if (prefs_.putString(kKeyMac, mac.c_str()) == 0 && !mac.empty()) return false;
  Lock lock(mutex_);
  cache_.targetMac = mac;
  return true;
}

bool Config::setWifi(const std::string& ssid, const std::string& pass) {
  if (!open_) return false;
  if (ssid.size() > kMaxSsid || pass.size() > kMaxPass) return false;
  WriteLock wlock(writeMux_);
  // putString returns 0 for an empty value (a valid "clear"), so only treat 0
  // as failure when we actually wrote a non-empty string.
  if ((prefs_.putString(kKeySsid, ssid.c_str()) == 0 && !ssid.empty()) ||
      (prefs_.putString(kKeyPass, pass.c_str()) == 0 && !pass.empty())) {
    loadCache();  // partial write: resync the cache with NVS
    return false;
  }
  Lock lock(mutex_);
  cache_.wifiSsid = ssid;
  cache_.wifiPass = pass;
  return true;
}

bool Config::clearWifi() { return setWifi("", ""); }

bool Config::setMqtt(const MqttSettings& in) {
  if (!open_) return false;
  MqttSettings m = in;
  if (m.baseTopic.empty()) m.baseTopic = "wraith";
  if (!mqttSettingsValid(m)) return false;

  WriteLock wlock(writeMux_);
  std::string storedCa;
  {
    Lock lock(mutex_);
    storedCa = cache_.mqtt.caPem;
  }
  // An empty value removes the key (loadCache() then yields "" for it), so a
  // failed clear (e.g. "remove the CA") is reported instead of silently kept.
  auto putStr = [&](const char* key, const std::string& v) {
    if (v.empty()) return !prefs_.isKey(key) || prefs_.remove(key);
    return prefs_.putString(key, v.c_str()) != 0;
  };
  // Order the ignition flag so a partial write failure can only leave NVS LESS
  // permissive: revoke before touching the broker keys, grant only after them
  // (otherwise a new broker could boot with the old "allowed" flag).
  const bool persisted = [&] {
    if (!m.allowRemoteIgnition && prefs_.putBool(kKeyAllowIgn, false) == 0) return false;
    if (prefs_.putBool(kKeyMqttEn, m.enabled) == 0) return false;
    if (!putStr(kKeyMqttHost, m.host)) return false;
    if (prefs_.putUShort(kKeyMqttPort, m.port) == 0) return false;
    if (!putStr(kKeyMqttTransport, m.transport)) return false;
    if (!putStr(kKeyMqttPath, m.path)) return false;
    if (!putStr(kKeyMqttUser, m.user)) return false;
    if (!putStr(kKeyMqttPass, m.pass)) return false;
    if (!putStr(kKeyMqttTopic, m.baseTopic)) return false;
    if (m.caPem != storedCa && !putStr(kKeyMqttCa, m.caPem)) return false;  // ~2 KB
    if (prefs_.putBool(kKeyMqttNoCn, m.skipHostnameCheck) == 0) return false;
    if (m.allowRemoteIgnition && prefs_.putBool(kKeyAllowIgn, true) == 0) return false;
    return true;
  }();
  if (!persisted) {
    // Partial write: make the cache show what NVS really holds, so later checks
    // (e.g. the remote-ignition password gate) never read a stale value.
    loadCache();
    return false;
  }

  Lock lock(mutex_);
  cache_.mqtt = m;
  return true;
}

bool Config::setTime(const std::string& timezone, const std::string& ntpServer) {
  if (!open_) return false;
  const std::string tz = timezone.empty() ? kDefaultTz : timezone;
  const std::string ntp = ntpServer.empty() ? "pool.ntp.org" : ntpServer;
  WriteLock wlock(writeMux_);
  ++timeGen_;  // any in-flight IP guess is now stale, even if a write fails
  if (prefs_.putString(kKeyTz, tz.c_str()) == 0 ||
      prefs_.putString(kKeyNtp, ntp.c_str()) == 0 ||
      prefs_.putBool(kKeyTzChosen, true) == 0) {
    loadCache();  // partial write: resync the cache with NVS
    return false;
  }
  Lock lock(mutex_);
  cache_.timezone = tz;
  cache_.ntpServer = ntp;
  cache_.timezoneChosen = true;
  return true;
}

uint32_t Config::timeGeneration() const { return timeGen_.load(); }

bool Config::applyGuessedTimezone(const std::string& timezone,
                                  uint32_t expectedGeneration) {
  if (!open_ || timezone.empty()) return false;
  WriteLock wlock(writeMux_);
  // Checked under the writer lock, so no setTime() can slip in before the write.
  if (timeGen_.load() != expectedGeneration) return false;  // user saved meanwhile
  if (prefs_.putString(kKeyTz, timezone.c_str()) == 0) return false;
  // A guess counts as chosen: the boot auto-guess stays one-shot.
  if (prefs_.putBool(kKeyTzChosen, true) == 0) return false;
  Lock lock(mutex_);
  cache_.timezone = timezone;
  cache_.timezoneChosen = true;
  return true;
}

bool Config::setHostname(const std::string& hostname) {
  if (!open_) return false;
  // Sanitise to a valid DNS/mDNS label: lowercase [a-z0-9-], max 32 chars, and
  // with no leading or trailing hyphen (an invalid DNS label).
  std::string clean;
  for (char c : hostname) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') clean += c;
    if (clean.size() >= 32) break;
  }
  while (!clean.empty() && clean.front() == '-') clean.erase(clean.begin());
  while (!clean.empty() && clean.back() == '-') clean.pop_back();
  WriteLock wlock(writeMux_);
  if (prefs_.putString(kKeyHost, clean.c_str()) == 0 && !clean.empty()) return false;
  Lock lock(mutex_);
  cache_.hostname = clean;  // "" => caller falls back to the auto name
  return true;
}

bool Config::factoryReset() {
  if (!open_) return false;
  WriteLock wlock(writeMux_);
  // Keep only the OTA anti-downgrade floor (documented as never lowering). The
  // device password goes too: a reset is how a forgotten one is recovered (it
  // also turns remote START off, so nothing it guarded stays enabled).
  uint32_t keepMinBuild;
  {
    Lock lock(mutex_);
    keepMinBuild = cache_.minBuild;
  }
  const bool ok = prefs_.clear();
  if (keepMinBuild != 0 && prefs_.putUInt(kKeyMinBuild, keepMinBuild) == 0) {
    log_e("config: failed to re-persist the OTA floor after reset");
  }
  // Reload from NVS so the cache shows exactly what is stored: defaults after a
  // good clear, the surviving values after a partial one.
  loadCache();
  ++timeGen_;  // time settings were reset; drop any in-flight IP guess
  return ok;
}

}  // namespace fogger
