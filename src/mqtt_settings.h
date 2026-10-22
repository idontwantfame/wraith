// mqtt_settings.h — MQTT broker settings + the pure rules that validate them.
//
// Framework-agnostic (host-testable, see test/test_mqtt_settings.cpp). Shared by
// Config (persistence), the web API (validation + gates) and MqttHa (client).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace fogger {

// NVS strings are limited to 4000 bytes and must fit one 4 KB page; a single CA
// certificate (PEM) is ~1.2-2 KB, so keep a comfortable margin.
constexpr size_t kMaxMqttCaPemBytes = 3000;
constexpr size_t kMaxMqttBaseTopic = 64;
constexpr size_t kMaxWebSocketPath = 64;

struct MqttSettings {
  bool enabled = false;
  std::string host;
  uint16_t port = 1883;
  std::string transport = "mqtt";  // mqtt | mqtts (TLS) | ws | wss (TLS)
  std::string path = "/mqtt";      // WebSocket path (ws / wss only)
  std::string user;
  std::string pass;
  std::string baseTopic = "wraith";
  std::string caPem;               // broker CA (PEM), required for mqtts / wss
  bool skipHostnameCheck = false;  // TLS: verify the CA, not the cert host name
  bool allowRemoteIgnition = false;
};

inline bool isValidMqttTransport(const std::string& t) {
  return t == "mqtt" || t == "mqtts" || t == "ws" || t == "wss";
}
inline bool mqttTransportIsTls(const std::string& t) { return t == "mqtts" || t == "wss"; }
inline bool mqttTransportIsWebSocket(const std::string& t) { return t == "ws" || t == "wss"; }

// Build capability: MQTT over TLS (mqtts / wss). A TLS session needs ~40 KB of
// heap with this core's fixed 16 KB mbedTLS record buffers — more than an
// ESP32-C3 has free alongside BLE + Wi-Fi + the web server — so it is opt-in
// per build (platformio.ini): -DWRAITH_MQTT_TLS=1 for a board with PSRAM, or a
// custom sdkconfig with dynamic TLS buffers. Off when unset.
#ifndef WRAITH_MQTT_TLS
#define WRAITH_MQTT_TLS 0
#endif
constexpr bool kMqttTlsSupported = WRAITH_MQTT_TLS != 0;

// A transport this build can actually use (TLS ones only with WRAITH_MQTT_TLS).
inline bool mqttTransportSupported(const std::string& t) {
  return isValidMqttTransport(t) && (kMqttTlsSupported || !mqttTransportIsTls(t));
}

// A WebSocket path: starts with '/', printable ASCII without spaces, <= 64 chars.
inline bool isValidWebSocketPath(const std::string& p) {
  if (p.empty() || p.size() > kMaxWebSocketPath || p[0] != '/') return false;
  for (char c : p) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u <= 0x20 || u >= 0x7F) return false;
  }
  return true;
}

// The base topic is spliced into publish/subscribe topics: no MQTT wildcards,
// spaces or control chars, no reserved leading '$', 1..64 chars.
inline bool isValidMqttBaseTopic(const std::string& t) {
  if (t.empty() || t.size() > kMaxMqttBaseTopic || t[0] == '$') return false;
  for (char c : t) {
    if (c == '+' || c == '#' || c == ' ' || static_cast<unsigned char>(c) < 0x20) return false;
  }
  return true;
}

// Shape check for a pasted CA certificate (the device also parses it with
// mbedTLS before saving): a PEM CERTIFICATE block within the size limit.
inline bool looksLikeCaPem(const std::string& pem) {
  if (pem.empty() || pem.size() > kMaxMqttCaPemBytes) return false;
  if (pem.find("PRIVATE KEY") != std::string::npos) return false;  // never store a key
  const size_t begin = pem.find("-----BEGIN CERTIFICATE-----");
  if (begin == std::string::npos) return false;
  for (size_t i = 0; i < begin; ++i) {
    if (pem[i] != ' ' && pem[i] != '\n' && pem[i] != '\r' && pem[i] != '\t') return false;
  }
  return pem.find("-----END CERTIFICATE-----", begin) != std::string::npos;
}

// Same broker account: the stored password may be reused. Never for another
// host/port/user, nor over another transport (TLS -> plain would expose it);
// the WebSocket path counts only for WebSocket transports. Over TLS the
// password is only as safe as the server check, so a new CA or a relaxed
// host-name check (which could let a spoofed broker in) is a new account too.
inline bool sameBrokerAccount(const MqttSettings& a, const MqttSettings& b) {
  if (a.host != b.host || a.port != b.port || a.user != b.user ||
      a.transport != b.transport) {
    return false;
  }
  if (mqttTransportIsWebSocket(a.transport) && a.path != b.path) return false;
  if (mqttTransportIsTls(a.transport) &&
      (a.caPem != b.caPem || a.skipHostnameCheck != b.skipHostnameCheck)) {
    return false;
  }
  return true;
}

// Whether the stored password may be kept when settings are saved with the
// password field blank. Separate from sameBrokerAccount (which also decides the
// ignition scope): the same host and user, and never onto a weaker transport.
// TLS -> plain would send it in the clear, and over TLS it is only as safe as
// the server check (same CA, host-name check not newly skipped). Port and
// WebSocket path may change: another listener of the same broker needs no
// retyping.
inline bool passwordCarriesOver(const MqttSettings& cur, const MqttSettings& next) {
  if (cur.host != next.host || cur.user != next.user) return false;
  if (!mqttTransportIsTls(cur.transport)) return true;    // from plain: never weaker
  if (!mqttTransportIsTls(next.transport)) return false;  // TLS -> plain
  return cur.caPem == next.caPem && (cur.skipHostnameCheck || !next.skipHostnameCheck);
}

// Same ignition scope: same account, topic and TLS trust settings. Anything
// else changes who can command the device (needs the device password while
// remote ignition is allowed).
inline bool sameIgnitionScope(const MqttSettings& a, const MqttSettings& b) {
  return sameBrokerAccount(a, b) && a.baseTopic == b.baseTopic &&
         a.caPem == b.caPem && a.skipHostnameCheck == b.skipHostnameCheck;
}

// Constant-time compare (only the length leaks), so response timing can't
// reveal how much of a guessed secret matched.
inline bool secretEquals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  return diff == 0;
}

constexpr size_t kMinDevicePasswordLen = 8;

enum class IgnitionAuth : uint8_t {
  kAllowed,        // nothing to authorise, or the device password matched
  kSetPassword,    // no device password yet: store the typed one, then allow
  kTooShort,       // no device password yet, and the typed one is too short
  kWrongPassword,  // a device password is set and the typed one doesn't match
};

// Remote START lets whoever can publish our command topic start fog from
// anywhere. Turning it on, or changing the broker / topic / TLS trust while it
// is on, needs the device password the user chose. The first time, with none
// stored yet, the typed password becomes it. (The web UI itself has no login:
// the LAN is trusted, see docs/ARCHITECTURE.md.)
inline IgnitionAuth authorizeIgnitionChange(const MqttSettings& cur, const MqttSettings& next,
                                            const std::string& stored,
                                            const std::string& typed) {
  if (!next.allowRemoteIgnition) return IgnitionAuth::kAllowed;
  if (cur.allowRemoteIgnition && sameIgnitionScope(cur, next)) return IgnitionAuth::kAllowed;
  if (stored.empty()) {
    return typed.size() >= kMinDevicePasswordLen ? IgnitionAuth::kSetPassword
                                                 : IgnitionAuth::kTooShort;
  }
  return secretEquals(typed, stored) ? IgnitionAuth::kAllowed : IgnitionAuth::kWrongPassword;
}

}  // namespace fogger
