// test_mqtt_settings.cpp — host unit tests for the MQTT settings rules
// (transport / path / topic / CA validation and the broker-identity checks that
// gate password reuse and remote ignition).
//
// Run: pio test -e native -e native_tls
#include <cstdio>
#include <string>

#include "mqtt_settings.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
      ++g_failures;                                                       \
    }                                                                     \
  } while (0)

using fogger::MqttSettings;

void testTransport() {
  CHECK(fogger::isValidMqttTransport("mqtt"));
  CHECK(fogger::isValidMqttTransport("mqtts"));
  CHECK(fogger::isValidMqttTransport("ws"));
  CHECK(fogger::isValidMqttTransport("wss"));
  CHECK(!fogger::isValidMqttTransport(""));
  CHECK(!fogger::isValidMqttTransport("MQTT"));
  CHECK(!fogger::isValidMqttTransport("http"));
  CHECK(!fogger::isValidMqttTransport("mqtt "));

  CHECK(fogger::mqttTransportIsTls("mqtts") && fogger::mqttTransportIsTls("wss"));
  CHECK(!fogger::mqttTransportIsTls("mqtt") && !fogger::mqttTransportIsTls("ws"));
  CHECK(fogger::mqttTransportIsWebSocket("ws") && fogger::mqttTransportIsWebSocket("wss"));
  CHECK(!fogger::mqttTransportIsWebSocket("mqtt") && !fogger::mqttTransportIsWebSocket("mqtts"));
}

// Run this suite with -DWRAITH_MQTT_TLS=0 and =1: plain + WebSocket are always
// available, the TLS transports only when the build enables them.
void testTransportSupport() {
  CHECK(fogger::mqttTransportSupported("mqtt"));
  CHECK(fogger::mqttTransportSupported("ws"));
  CHECK(fogger::mqttTransportSupported("mqtts") == fogger::kMqttTlsSupported);
  CHECK(fogger::mqttTransportSupported("wss") == fogger::kMqttTlsSupported);
  CHECK(!fogger::mqttTransportSupported("http"));
}

void testWebSocketPath() {
  CHECK(fogger::isValidWebSocketPath("/mqtt"));
  CHECK(fogger::isValidWebSocketPath("/"));
  CHECK(fogger::isValidWebSocketPath("/ws/mqtt?x=1"));
  CHECK(!fogger::isValidWebSocketPath(""));
  CHECK(!fogger::isValidWebSocketPath("mqtt"));        // must start with '/'
  CHECK(!fogger::isValidWebSocketPath("/a b"));        // no spaces
  CHECK(!fogger::isValidWebSocketPath("/a\x01"));      // no control chars
  CHECK(!fogger::isValidWebSocketPath("/\x80"));       // printable ASCII only
  CHECK(!fogger::isValidWebSocketPath("/" + std::string(64, 'a')));  // > 64 chars
  CHECK(fogger::isValidWebSocketPath("/" + std::string(63, 'a')));   // = 64 chars
}

void testBaseTopic() {
  CHECK(fogger::isValidMqttBaseTopic("wraith"));
  CHECK(fogger::isValidMqttBaseTopic("home/fog"));
  CHECK(!fogger::isValidMqttBaseTopic(""));
  CHECK(!fogger::isValidMqttBaseTopic("a+b"));
  CHECK(!fogger::isValidMqttBaseTopic("a/#"));
  CHECK(!fogger::isValidMqttBaseTopic("a b"));
  CHECK(!fogger::isValidMqttBaseTopic("a\x01"));
  CHECK(!fogger::isValidMqttBaseTopic("$SYS"));
  CHECK(!fogger::isValidMqttBaseTopic(std::string(65, 'a')));
  CHECK(fogger::isValidMqttBaseTopic(std::string(64, 'a')));
}

void testCaPemShape() {
  const std::string pem =
      "-----BEGIN CERTIFICATE-----\nMIIB...\n-----END CERTIFICATE-----\n";
  CHECK(fogger::looksLikeCaPem(pem));
  CHECK(fogger::looksLikeCaPem("  \n" + pem));  // leading whitespace tolerated
  CHECK(!fogger::looksLikeCaPem(""));
  CHECK(!fogger::looksLikeCaPem("hello"));
  CHECK(!fogger::looksLikeCaPem("-----BEGIN PRIVATE KEY-----\nabc\n-----END PRIVATE KEY-----\n"));
  CHECK(!fogger::looksLikeCaPem("-----BEGIN CERTIFICATE-----\nno end marker\n"));
  // Over the NVS-safe size limit.
  CHECK(!fogger::looksLikeCaPem("-----BEGIN CERTIFICATE-----\n" +
                                std::string(fogger::kMaxMqttCaPemBytes, 'A') +
                                "\n-----END CERTIFICATE-----\n"));
}

MqttSettings base() {
  MqttSettings s;
  s.enabled = true;
  s.host = "192.168.0.24";
  s.port = 1883;
  s.transport = "mqtt";
  s.path = "/mqtt";
  s.user = "fog";
  s.pass = "secret";
  s.baseTopic = "wraith";
  return s;
}

void testIgnitionAuth() {
  using fogger::IgnitionAuth;
  const MqttSettings off = base();
  MqttSettings on = base();
  on.allowRemoteIgnition = true;

  // Remote START staying off (or being turned off) needs nothing.
  CHECK(fogger::authorizeIgnitionChange(off, off, "", "") == IgnitionAuth::kAllowed);
  CHECK(fogger::authorizeIgnitionChange(on, off, "hunter22", "") == IgnitionAuth::kAllowed);

  // First time on with no device password stored: the typed one becomes it,
  // if it is long enough.
  CHECK(fogger::authorizeIgnitionChange(off, on, "", "hunter22") == IgnitionAuth::kSetPassword);
  CHECK(fogger::authorizeIgnitionChange(off, on, "", "short") == IgnitionAuth::kBadLength);
  CHECK(fogger::authorizeIgnitionChange(off, on, "", "") == IgnitionAuth::kBadLength);
  CHECK(fogger::authorizeIgnitionChange(off, on, "", std::string(65, 'x')) ==
        IgnitionAuth::kBadLength);
  CHECK(fogger::authorizeIgnitionChange(off, on, "", std::string(64, 'x')) ==
        IgnitionAuth::kSetPassword);

  // With one stored, it must match.
  CHECK(fogger::authorizeIgnitionChange(off, on, "hunter22", "hunter22") ==
        IgnitionAuth::kAllowed);
  CHECK(fogger::authorizeIgnitionChange(off, on, "hunter22", "hunter2") ==
        IgnitionAuth::kWrongPassword);

  // Already on and the same ignition scope (e.g. only the MQTT password changed):
  // no device password needed. Another broker / topic: needed again.
  MqttSettings samescope = on;
  samescope.pass = "new";
  CHECK(fogger::authorizeIgnitionChange(on, samescope, "hunter22", "") ==
        IgnitionAuth::kAllowed);
  MqttSettings moved = on;
  moved.host = "10.0.0.9";
  CHECK(fogger::authorizeIgnitionChange(on, moved, "hunter22", "") ==
        IgnitionAuth::kWrongPassword);
  CHECK(fogger::authorizeIgnitionChange(on, moved, "hunter22", "hunter22") ==
        IgnitionAuth::kAllowed);
}

void testPasswordCarriesOver() {
  const MqttSettings a = base();
  MqttSettings b = base();
  CHECK(fogger::passwordCarriesOver(a, b));

  // Same host and user: a new port, path or a plain -> WebSocket switch keeps it,
  // so moving to another listener of the same broker needs no retyping.
  b = base(); b.port = 1884; b.transport = "ws";
  CHECK(fogger::passwordCarriesOver(a, b));
  b.path = "/other";                 CHECK(fogger::passwordCarriesOver(a, b));
  b = base(); b.transport = "mqtts"; b.port = 8883;
  CHECK(fogger::passwordCarriesOver(a, b));  // plain -> TLS is stronger

  // Never to another host or user.
  b = base(); b.host = "10.0.0.1";   CHECK(!fogger::passwordCarriesOver(a, b));
  b = base(); b.user = "other";      CHECK(!fogger::passwordCarriesOver(a, b));

  // Never from TLS to plain (it would travel in the clear), nor to a TLS setup
  // that trusts another CA or newly skips the host-name check.
  MqttSettings ta = base(); ta.transport = "mqtts"; ta.caPem = "CA1";
  MqttSettings tb = ta;      tb.transport = "mqtt";
  CHECK(!fogger::passwordCarriesOver(ta, tb));
  tb = ta; tb.transport = "ws";      CHECK(!fogger::passwordCarriesOver(ta, tb));
  tb = ta; tb.transport = "wss";     CHECK(fogger::passwordCarriesOver(ta, tb));
  tb = ta; tb.caPem = "CA2";         CHECK(!fogger::passwordCarriesOver(ta, tb));
  tb = ta; tb.skipHostnameCheck = true;
  CHECK(!fogger::passwordCarriesOver(ta, tb));
  MqttSettings sa = ta; sa.skipHostnameCheck = true;
  CHECK(fogger::passwordCarriesOver(sa, ta));  // turning the check back on is fine
}

void testBrokerIdentity() {
  const MqttSettings a = base();
  MqttSettings b = base();
  CHECK(fogger::sameBrokerAccount(a, b));
  CHECK(fogger::sameIgnitionScope(a, b));

  // The password is never reused for a different host/port/user, nor over a
  // different transport (e.g. TLS -> plain would send it in the clear).
  b = base(); b.host = "10.0.0.1";   CHECK(!fogger::sameBrokerAccount(a, b));
  b = base(); b.port = 8883;         CHECK(!fogger::sameBrokerAccount(a, b));
  b = base(); b.user = "other";      CHECK(!fogger::sameBrokerAccount(a, b));
  b = base(); b.transport = "mqtts"; CHECK(!fogger::sameBrokerAccount(a, b));

  // The WebSocket path only matters for WebSocket transports.
  b = base(); b.path = "/other";     CHECK(fogger::sameBrokerAccount(a, b));
  MqttSettings wa = base(); wa.transport = "ws";
  MqttSettings wb = wa;   wb.path = "/other";
  CHECK(!fogger::sameBrokerAccount(wa, wb));

  // Changing the topic or the TLS trust settings changes who can command the
  // device, so it leaves the ignition scope (but not the account).
  b = base(); b.baseTopic = "x";
  CHECK(fogger::sameBrokerAccount(a, b) && !fogger::sameIgnitionScope(a, b));
  MqttSettings ta = base(); ta.transport = "mqtts"; ta.caPem = "CA1";
  MqttSettings tb = ta;
  CHECK(fogger::sameIgnitionScope(ta, tb));
  tb.caPem = "CA2";                  CHECK(!fogger::sameIgnitionScope(ta, tb));
  tb = ta; tb.skipHostnameCheck = true;
  CHECK(!fogger::sameIgnitionScope(ta, tb));

  // Over TLS the password is only as safe as the server check: a new CA or a
  // relaxed host-name check must not reuse it (a spoofed broker could get it).
  tb = ta; tb.caPem = "CA2";          CHECK(!fogger::sameBrokerAccount(ta, tb));
  tb = ta; tb.skipHostnameCheck = true; CHECK(!fogger::sameBrokerAccount(ta, tb));
  // Without TLS the CA plays no part in the connection.
  b = base(); b.caPem = "CA9";        CHECK(fogger::sameBrokerAccount(a, b));
}

void testCaPemRejectsKeys() {
  // A pasted private key (even after a valid certificate) is never stored.
  const std::string pem =
      "-----BEGIN CERTIFICATE-----\nMIIB...\n-----END CERTIFICATE-----\n";
  CHECK(!fogger::looksLikeCaPem(pem + "-----BEGIN PRIVATE KEY-----\nx\n-----END PRIVATE KEY-----\n"));
  CHECK(!fogger::looksLikeCaPem(pem + "-----BEGIN RSA PRIVATE KEY-----\nx\n"));
}

}  // namespace

int main() {
  testTransport();
  testTransportSupport();
  testWebSocketPath();
  testBaseTopic();
  testCaPemShape();
  testBrokerIdentity();
  testPasswordCarriesOver();
  testIgnitionAuth();
  testCaPemRejectsKeys();
  if (g_failures == 0) {
    std::printf("OK: all MQTT settings tests passed\n");
    return 0;
  }
  std::printf("%d assertion(s) failed\n", g_failures);
  return 1;
}
