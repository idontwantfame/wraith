// mqtt_ha.cpp — MQTT (esp-mqtt) + Home Assistant discovery. See mqtt_ha.h.
#include "mqtt_ha.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string_view>

#include "esp_system.h"
#include "fogger_protocol.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"

namespace fogger {
namespace {

constexpr int kMqttBufferSize = 1024;        // room for HA discovery payloads
constexpr char kDiscoveryPrefix[] = "homeassistant";
constexpr uint32_t kClientStackBytes = 10240;    // esp-mqtt task: TLS + discovery
constexpr uint32_t kPublisherStackBytes = 6144;  // state JSON + publish
// Same priority as loop(): a TLS handshake (seconds of crypto on the C3) then
// time-slices with loop() instead of starving it (esp-mqtt's default is 5).
constexpr UBaseType_t kTaskPriority = 1;
constexpr uint32_t kPublisherPollMs = 100;
// Fresh telemetry goes out at most once a second: status frames arrive 4-10
// times a second, and publishing each one churns the heap (and the broker).
constexpr uint32_t kMinStatePublishMs = 1000;
constexpr int kKeepaliveSec = 30;
constexpr int kNetworkTimeoutMs = 10000;
constexpr int kMaxCommandBytes = 64;  // our command payloads are tiny

SemaphoreHandle_t asMutex(void* h) { return static_cast<SemaphoreHandle_t>(h); }
esp_mqtt_client_handle_t asClient(void* h) { return static_cast<esp_mqtt_client_handle_t>(h); }

// RAII scoped lock over a FreeRTOS mutex (no-op before begin() creates it).
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

esp_mqtt_transport_t transportFor(const std::string& t) {
  if (t == "mqtts") return MQTT_TRANSPORT_OVER_SSL;
  if (t == "ws") return MQTT_TRANSPORT_OVER_WS;
  if (t == "wss") return MQTT_TRANSPORT_OVER_WSS;
  return MQTT_TRANSPORT_OVER_TCP;
}

// Case-insensitive match against an upper-case word, without allocating.
bool equalsWord(std::string_view s, std::string_view upperWord) {
  if (s.size() != upperWord.size()) return false;
  for (size_t i = 0; i < s.size(); ++i) {
    if (toupper(static_cast<unsigned char>(s[i])) != upperWord[i]) return false;
  }
  return true;
}

// Fill the fields every HA discovery config shares (identity + availability +
// the device block that groups all entities under one HA device).
void addCommon(JsonDocument& doc, const MqttHaConfig& cfg, const std::string& name,
               const std::string& object, const std::string& stateTopic,
               const std::string& availTopic) {
  doc["name"] = name;
  doc["uniq_id"] = cfg.deviceId + "_" + object;
  doc["stat_t"] = stateTopic;
  doc["avail_t"] = availTopic;
  doc["pl_avail"] = "online";
  doc["pl_not_avail"] = "offline";
  JsonObject dev = doc["dev"].to<JsonObject>();
  dev["ids"][0] = cfg.deviceId;
  dev["name"] = cfg.deviceName;
  dev["mf"] = "Filmog / E-TIMING";
  dev["mdl"] = "CH9141BLE2U (Wraith controller)";
}

}  // namespace

void MqttHa::onCommands(FogFn fogStart, FogFn fogStop, DensityFn density, FanFn fan,
                        FogFn powerOff) {
  fogStart_ = std::move(fogStart);
  fogStop_ = std::move(fogStop);
  density_ = std::move(density);
  fan_ = std::move(fan);
  powerOff_ = std::move(powerOff);
}

std::string MqttHa::deviceTopic(const char* leaf) const {
  return cfg_.broker.baseTopic + "/" + cfg_.deviceId + "/" + leaf;
}

bool MqttHa::begin(const MqttHaConfig& cfg) {
  cfg_ = cfg;
  const MqttSettings& b = cfg_.broker;
  if (!b.enabled) {
    log_i("mqtt: disabled");
    return true;  // not an error; simply inactive
  }
  mux_.store(xSemaphoreCreateMutex());
  if (mux_.load() == nullptr) {
    log_e("mqtt: failed to create lock");
    return false;
  }
  if (!mqttTransportSupported(b.transport)) {
    setError("TLS isn't available in this firmware build");
    log_w("mqtt: %s", lastError().c_str());
    return false;
  }
  if (b.host.empty() || (mqttTransportIsTls(b.transport) && b.caPem.empty())) {
    setError(b.host.empty() ? "no broker host set" : "TLS needs the broker's CA certificate");
    log_w("mqtt: %s", lastError().c_str());
    return false;
  }
  availTopic_ = deviceTopic("status");
  fogSetTopic_ = deviceTopic("fog/set");
  densitySetTopic_ = deviceTopic("density/set");
  fanSetTopic_ = deviceTopic("fan/set");
  powerSetTopic_ = deviceTopic("power/set");

  esp_mqtt_client_config_t mc = {};
  mc.broker.address.hostname = b.host.c_str();
  mc.broker.address.port = b.port;
  mc.broker.address.transport = transportFor(b.transport);
  if (mqttTransportIsWebSocket(b.transport)) mc.broker.address.path = b.path.c_str();
  if (mqttTransportIsTls(b.transport)) {
    mc.broker.verification.certificate = b.caPem.c_str();  // not copied: cfg_ lives on
    mc.broker.verification.skip_cert_common_name_check = b.skipHostnameCheck;
  }
  mc.credentials.client_id = cfg_.deviceId.c_str();
  if (!b.user.empty()) mc.credentials.username = b.user.c_str();
  if (!b.pass.empty()) mc.credentials.authentication.password = b.pass.c_str();
  mc.session.last_will.topic = availTopic_.c_str();
  mc.session.last_will.msg = "offline";
  mc.session.last_will.qos = 0;
  mc.session.last_will.retain = 1;
  mc.session.keepalive = kKeepaliveSec;
  mc.network.reconnect_timeout_ms = static_cast<int>(cfg_.reconnectMs);
  mc.network.timeout_ms = kNetworkTimeoutMs;
  mc.task.priority = kTaskPriority;
  mc.task.stack_size = kClientStackBytes;
  mc.buffer.size = kMqttBufferSize;
  mc.buffer.out_size = kMqttBufferSize;

  esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mc);
  if (client == nullptr) {
    log_e("mqtt: client init failed");
    return false;
  }
  client_ = client;
  esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, &MqttHa::onEvent, this);
  started_ = true;
  if (esp_mqtt_client_start(client) != ESP_OK) {
    started_ = false;
    log_e("mqtt: client start failed");
    return false;
  }
  if (xTaskCreate(&MqttHa::publisherEntry, "mqttpub", kPublisherStackBytes, this,
                  kTaskPriority, nullptr) != pdPASS) {
    log_e("mqtt: publisher task failed; telemetry won't be published");
  }
  log_i("mqtt: %s://%s:%u base=%s id=%s", b.transport.c_str(), b.host.c_str(), b.port,
        b.baseTopic.c_str(), cfg_.deviceId.c_str());
  return true;
}

void MqttHa::onEvent(void* self, const char* /*base*/, int32_t id, void* data) {
  auto* mqtt = static_cast<MqttHa*>(self);
  // Firmware is built with exceptions: an uncaught bad_alloc here would abort
  // the chip, and with it the BLE loop and any STOP. Drop the event instead.
  try {
    switch (static_cast<esp_mqtt_event_id_t>(id)) {
      case MQTT_EVENT_CONNECTED:
        mqtt->onConnected();
        break;
      case MQTT_EVENT_DISCONNECTED:
        mqtt->connected_.store(false);
        log_w("mqtt: disconnected; retrying");
        break;
      case MQTT_EVENT_DATA:
        mqtt->onData(data);
        break;
      case MQTT_EVENT_ERROR:
        mqtt->onError(data);
        break;
      default:
        break;
    }
  } catch (const std::bad_alloc&) {
    log_w("mqtt: out of memory handling event %d; dropped", static_cast<int>(id));
  }
}

void MqttHa::onConnected() {
  setError("");
  log_i("mqtt: connected to broker");
  publish(availTopic_, "online", /*retain=*/true);
  // Discovery is the big allocation here; if it runs out of memory, still
  // subscribe below, or STOP / power-off over MQTT would be lost until the next
  // reconnect.
  try {
    publishDiscovery();
  } catch (const std::bad_alloc&) {
    log_w("mqtt: out of memory publishing discovery; continuing");
  }
  // Delete retained commands at the broker before subscribing (an empty retained
  // publish clears one). Any that still arrive are dropped by their retain flag.
  bool subscribed = true;
  for (const std::string* topic :
       {&fogSetTopic_, &densitySetTopic_, &fanSetTopic_, &powerSetTopic_}) {
    publish(*topic, "", /*retain=*/true);
    subscribed &= esp_mqtt_client_subscribe_single(asClient(client_), topic->c_str(), 0) >= 0;
  }
  if (!subscribed) setError("connected, but subscribing to the command topics failed");
  connected_.store(true);
  statePending_.store(true);  // the publisher task sends the current state now

  static bool resourcesLogged = false;  // once: after TLS + discovery, the peak
  if (!resourcesLogged) {
    resourcesLogged = true;
    log_i("mqtt: client stack headroom %u bytes, free heap %u bytes (min %u)",
          (unsigned)uxTaskGetStackHighWaterMark(nullptr), (unsigned)esp_get_free_heap_size(),
          (unsigned)esp_get_minimum_free_heap_size());
  }
}

void MqttHa::onData(void* event) {
  const auto* ev = static_cast<esp_mqtt_event_handle_t>(event);
  // Our commands are tiny: a fragmented / oversized message is never one of ours.
  if (ev->topic == nullptr || ev->current_data_offset != 0 ||
      ev->data_len != ev->total_data_len || ev->data_len > kMaxCommandBytes) {
    return;
  }
  onMessage(std::string_view(ev->topic, ev->topic_len),
            std::string_view(ev->data, ev->data_len), ev->retain);
}

void MqttHa::onError(void* event) {
  const auto* ev = static_cast<esp_mqtt_event_handle_t>(event);
  const esp_mqtt_error_codes_t* e = ev->error_handle;
  if (e == nullptr) return;
  char msg[96];
  if (e->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
    snprintf(msg, sizeof(msg), "broker refused the login (code %d): check user/password",
             (int)e->connect_return_code);
  } else if (e->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
    if (e->esp_tls_cert_verify_flags != 0 && cfg_.time != nullptr && !cfg_.time->synced()) {
      // Certificates can't be checked against a 1970 clock: every one looks
      // "not yet valid" until NTP has set the time.
      snprintf(msg, sizeof(msg), "TLS: waiting for the clock (NTP) to verify the certificate");
    } else if (e->esp_tls_cert_verify_flags != 0) {
      snprintf(msg, sizeof(msg), "TLS: broker certificate not trusted (flags 0x%x)",
               (unsigned)e->esp_tls_cert_verify_flags);
    } else if (e->esp_tls_stack_err != 0) {
      snprintf(msg, sizeof(msg), "TLS handshake failed (error %d)", e->esp_tls_stack_err);
    } else {
      snprintf(msg, sizeof(msg), "can't reach the broker (errno %d)", e->esp_transport_sock_errno);
    }
  } else {
    snprintf(msg, sizeof(msg), "MQTT error (type %d)", (int)e->error_type);
  }
  setError(msg);
  log_w("mqtt: %s", msg);
}

void MqttHa::setError(const std::string& msg) {
  Lock lock(mux_.load());
  lastError_ = msg;
}

std::string MqttHa::lastError() const {
  void* m = mux_.load();
  if (m == nullptr) return std::string();  // not started: nothing written yet
  Lock lock(m);
  return lastError_;
}

bool MqttHa::publish(const std::string& topic, const std::string& payload, bool retain) {
  return esp_mqtt_client_publish(asClient(client_), topic.c_str(), payload.c_str(),
                                 static_cast<int>(payload.size()), /*qos=*/0,
                                 retain ? 1 : 0) >= 0;
}

void MqttHa::publisherEntry(void* self) { static_cast<MqttHa*>(self)->runPublisher(); }

void MqttHa::runPublisher() {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(kPublisherPollMs));
    if (!connected_.load()) continue;
    // Fresh telemetry at most once a second, plus a periodic heartbeat. The
    // flag is cleared before publishCurrentState() copies the snapshot, so a
    // frame landing in between is still in what goes out.
    const uint32_t now = millis();
    const bool heartbeatDue = (int32_t)(now - nextPublishMs_) >= 0;
    const bool freshDue = statePending_.load() && now - lastPublishMs_ >= kMinStatePublishMs;
    if (freshDue || heartbeatDue) {
      statePending_.store(false);
      publishCurrentState();
    }
  }
}

void MqttHa::publishDiscoveryEntity(const char* component, const char* object,
                                    const std::string& configJson) {
  std::string topic = std::string(kDiscoveryPrefix) + "/" + component + "/" +
                      cfg_.deviceId + "/" + object + "/config";
  if (!publish(topic, configJson, /*retain=*/true)) {
    log_w("mqtt: discovery publish failed for %s", object);
  }
}

void MqttHa::publishDiscovery() {
  const std::string stateTopic = deviceTopic("state");

  auto emit = [&](const char* component, const char* object, const char* name,
                  std::function<void(JsonDocument&)> extra) {
    JsonDocument doc;
    addCommon(doc, cfg_, name, object, stateTopic, availTopic_);
    extra(doc);
    std::string out;
    serializeJson(doc, out);
    publishDiscoveryEntity(component, object, out);
  };
  // An empty retained config deletes an entity from HA.
  auto remove = [&](const char* component, const char* object) {
    publishDiscoveryEntity(component, object, std::string());
  };

  // Controls follow the remote-control setting, so HA never shows a switch or
  // slider the device would ignore. Changing the setting reboots the device, and
  // this runs on every connect, so the other shape is removed from HA here.
  if (cfg_.broker.allowRemoteIgnition) {
    emit("switch", "fog", "Fog", [&](JsonDocument& d) {
      d["cmd_t"] = deviceTopic("fog/set");
      d["val_tpl"] = "{{ value_json.fog }}";
      d["stat_on"] = "ON";
      d["stat_off"] = "OFF";
      d["pl_on"] = "ON";
      d["pl_off"] = "OFF";
      d["ic"] = "mdi:weather-fog";
    });
    emit("number", "density", "Density", [&](JsonDocument& d) {
      d["cmd_t"] = deviceTopic("density/set");
      d["val_tpl"] = "{{ value_json.density }}";
      d["min"] = kDensityMin;
      d["max"] = kDensityMax;
      d["step"] = 1000;
      d["mode"] = "slider";
      d["ic"] = "mdi:cloud";
    });
    emit("number", "fan", "Fan", [&](JsonDocument& d) {
      d["cmd_t"] = deviceTopic("fan/set");
      d["val_tpl"] = "{{ value_json.fan }}";
      d["min"] = kFanMin;
      d["max"] = kFanMax;
      d["step"] = 5;
      d["mode"] = "slider";
      d["ic"] = "mdi:fan";
    });
    remove("sensor", "density_ro");
    remove("sensor", "fan_ro");
  } else {
    remove("switch", "fog");
    remove("number", "density");
    remove("number", "fan");
    emit("sensor", "density_ro", "Density", [&](JsonDocument& d) {
      d["val_tpl"] = "{{ value_json.density }}";
      d["ic"] = "mdi:cloud";
    });
    emit("sensor", "fan_ro", "Fan", [&](JsonDocument& d) {
      d["val_tpl"] = "{{ value_json.fan }}";
      d["unit_of_meas"] = "%";
      d["ic"] = "mdi:fan";
    });
  }
  // STOP is always accepted, so it is always offered (a button: no state).
  emit("button", "stop", "Stop fog", [&](JsonDocument& d) {
    d.remove("stat_t");  // buttons take no state topic
    d["cmd_t"] = deviceTopic("fog/set");
    d["pl_prs"] = "OFF";
    d["ic"] = "mdi:stop-circle";
  });
  // Power-off ends fog too, so it is always offered. One-way: afterwards only
  // the machine's own button switches it on.
  emit("button", "power_off", "Power off machine", [&](JsonDocument& d) {
    d.remove("stat_t");
    d["cmd_t"] = deviceTopic("power/set");
    d["pl_prs"] = "OFF";
    d["ic"] = "mdi:power";
  });
  emit("binary_sensor", "fogging", "Fogging", [&](JsonDocument& d) {
    d["val_tpl"] = "{{ value_json.fog }}";
    d["pl_on"] = "ON";
    d["pl_off"] = "OFF";
    d["dev_cla"] = "running";
  });
  emit("sensor", "battery", "Battery", [&](JsonDocument& d) {
    d["val_tpl"] = "{{ value_json.batt_min }}";
    d["dev_cla"] = "battery";
    d["unit_of_meas"] = "%";
  });
  emit("sensor", "battery_a", "Battery A", [&](JsonDocument& d) {
    d["val_tpl"] = "{{ value_json.batt_a }}";
    d["dev_cla"] = "battery";
    d["unit_of_meas"] = "%";
    d["ent_cat"] = "diagnostic";
  });
  emit("sensor", "battery_b", "Battery B", [&](JsonDocument& d) {
    d["val_tpl"] = "{{ value_json.batt_b }}";
    d["dev_cla"] = "battery";
    d["unit_of_meas"] = "%";
    d["ent_cat"] = "diagnostic";
  });
  emit("binary_sensor", "charger", "Charger", [&](JsonDocument& d) {
    d["val_tpl"] = "{{ value_json.charger }}";
    d["pl_on"] = "true";
    d["pl_off"] = "false";
    d["dev_cla"] = "plug";
  });
  // The machine's report word: events as well as faults (docs/PROTOCOL.md). The
  // object id stays "fault" so existing Home Assistant setups keep the entity.
  emit("sensor", "fault", "Machine report", [&](JsonDocument& d) {
    d["val_tpl"] = "{{ value_json.fault }}";
    d["ent_cat"] = "diagnostic";
    d["ic"] = "mdi:alert-circle";
  });
  emit("sensor", "rssi", "BLE RSSI", [&](JsonDocument& d) {
    d["val_tpl"] = "{{ value_json.rssi }}";
    d["dev_cla"] = "signal_strength";
    d["unit_of_meas"] = "dBm";
    d["ent_cat"] = "diagnostic";
  });
  emit("sensor", "link", "BLE link", [&](JsonDocument& d) {
    d["val_tpl"] = "{{ value_json.link }}";
    d["ent_cat"] = "diagnostic";
    d["ic"] = "mdi:bluetooth";
  });
  emit("sensor", "updated", "Last update", [&](JsonDocument& d) {
    d["val_tpl"] = "{{ value_json.time }}";
    d["dev_cla"] = "timestamp";
    d["ent_cat"] = "diagnostic";
  });
}

std::string MqttHa::stateJson(const FoggerSnapshot& snap) const {
  const StatusFrame& f = snap.status;
  char fault[8];
  snprintf(fault, sizeof(fault), "0x%04x", f.faultMirror);

  JsonDocument doc;
  doc["fog"] = f.isFogging() ? "ON" : "OFF";
  doc["density"] = f.density;
  doc["fan"] = f.fan;
  doc["batt_min"] = f.batteryMin();
  doc["batt_a"] = f.batteryA;
  doc["batt_b"] = f.batteryB;
  doc["charger"] = f.chargerPresent ? "true" : "false";
  doc["charge_done"] = f.chargeComplete ? "true" : "false";
  doc["fault"] = fault;
  doc["link"] = linkStateName(snap.link);
  doc["rssi"] = snap.rssi;
  doc["connected"] = snap.connected;
  if (cfg_.time != nullptr && cfg_.time->synced()) {
    doc["time"] = cfg_.time->isoUtc();  // ISO-8601, HA timestamp device_class
  }

  std::string out;
  serializeJson(doc, out);
  return out;
}

void MqttHa::publishState(const FoggerSnapshot& snap) {
  if (!started_) return;
  {
    Lock lock(mux_.load());
    lastSnap_ = snap;
  }
  statePending_.store(true);  // the publisher task sends it on its next pass
}

void MqttHa::publishCurrentState() {
  FoggerSnapshot snap;
  {
    Lock lock(mux_.load());
    snap = lastSnap_;
  }
  lastPublishMs_ = millis();
  nextPublishMs_ = lastPublishMs_ + cfg_.publishIntervalMs;
  // An uncaught bad_alloc on this task would abort the whole chip.
  bool sent = false;
  try {
    sent = publish(deviceTopic("state"), stateJson(snap), /*retain=*/true);
    if (!sent) log_w("mqtt: state publish failed");
  } catch (const std::bad_alloc&) {
    log_w("mqtt: out of memory; state publish skipped");
  }
  if (!sent) statePending_.store(true);  // try again next second, not at the heartbeat
}

// Allocation-free (topics are prebuilt in begin()): a STOP or power-off must
// not be lost to a failed allocation.
void MqttHa::onMessage(std::string_view topic, std::string_view payload, bool retained) {
  const bool isFog = topic == fogSetTopic_;
  const bool isDensity = topic == densitySetTopic_;
  const bool isFan = topic == fanSetTopic_;
  const bool isPower = topic == powerSetTopic_;
  if (!isFog && !isDensity && !isFan && !isPower) return;
  // A retained command is stale (replayed by the broker on subscribe, not sent
  // now): never act on one, so a stored ON can't re-ignite fog on a reconnect.
  if (retained) {
    log_w("mqtt: ignoring retained command on %.*s", static_cast<int>(topic.size()),
          topic.data());
    return;
  }
  if (isFog) {
    if (equalsWord(payload, "ON")) {
      if (cfg_.broker.allowRemoteIgnition && fogStart_) {
        fogStart_();
      } else {
        log_w("mqtt: remote fog START ignored (remote control is off)");
      }
    } else if (equalsWord(payload, "OFF") && fogStop_) {
      fogStop_();
    }
    return;
  }
  // Power-off ends fog, so like STOP it doesn't need remote control allowed.
  if (isPower) {
    if (equalsWord(payload, "OFF") && powerOff_) powerOff_();
    return;
  }
  // Density/fan change what the machine does, so like START they need remote
  // control to be allowed (a broker repointed from the LAN can't retune a run).
  if (!cfg_.broker.allowRemoteIgnition) {
    log_w("mqtt: remote density/fan ignored (remote control is off)");
    return;
  }
  char digits[kMaxCommandBytes + 1];  // strtol needs a terminated copy
  const size_t n = std::min(payload.size(), static_cast<size_t>(kMaxCommandBytes));
  memcpy(digits, payload.data(), n);
  digits[n] = '\0';
  char* end = nullptr;
  long v = strtol(digits, &end, 10);  // reject non-numeric/negative
  if (end == digits || v < 0) return;
  if (isDensity && density_) {
    density_(clampU32(static_cast<uint32_t>(v), kDensityMin, kDensityMax));
  } else if (isFan && fan_) {
    if (v > 255) v = 255;
    fan_(static_cast<uint8_t>(v));  // the builder clamps to [35,100]
  }
}

}  // namespace fogger
