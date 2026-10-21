// mqtt_ha.h — MQTT client with Home Assistant MQTT Discovery.
//
// Publishes telemetry and (optionally) accepts commands, and advertises itself
// to Home Assistant so entities appear automatically: "Stop fog" and "Power off
// machine" buttons, a fogging sensor, battery / charger / fault / RSSI sensors,
// and density / fan — as a fog switch + sliders only when remote control is
// allowed, otherwise as read-only sensors (the switch/sliders are then removed
// from HA).
//
// Safety: remote fog START, density and fan are honoured only when
// allowRemoteIgnition is set, so an HA automation (or a broker repointed from
// the LAN) cannot ignite or retune fog unattended. STOP and power-off end fog,
// so they are always accepted.
// Retained command messages (stale, replayed by the broker on subscribe) are
// never acted on.
//
// Transport: ESP-IDF's esp-mqtt client — plain MQTT, MQTT over TLS, MQTT over
// WebSocket or secure WebSocket (MqttSettings::transport). TLS verifies the
// broker against the configured CA certificate.
//
// Threading: esp-mqtt runs its own task (connect / reconnect / TLS / inbound
// dispatch) and a small publisher task sends telemetry; neither runs on loop(),
// so a slow or unreachable broker never delays a BLE STOP. Commands reach the
// BLE layer through its thread-safe request API.
#pragma once

#include <Arduino.h>

#include <atomic>
#include <functional>
#include <string>

#include "fogger_ble.h"
#include "mqtt_settings.h"
#include "timekeeper.h"

namespace fogger {

struct MqttHaConfig {
  MqttSettings broker;                          // what to connect to, and how
  std::string deviceId = "wraith";              // unique per controller
  std::string deviceName = "Wraith Fog Controller";
  uint32_t reconnectMs = 5000;                  // broker reconnect backoff
  uint32_t publishIntervalMs = 5000;            // heartbeat state publish
  TimeKeeper* time = nullptr;                   // optional; adds a timestamp
};

class MqttHa {
 public:
  // Command hooks wired by the app to the BLE layer. Each returns success. They
  // run on the esp-mqtt task, so they must be thread-safe (the BLE API is).
  using FogFn = std::function<bool()>;
  using DensityFn = std::function<bool(uint32_t)>;
  using FanFn = std::function<bool(uint8_t)>;

  // Wire before begin(); not changed afterwards.
  void onCommands(FogFn fogStart, FogFn fogStop, DensityFn density, FanFn fan,
                  FogFn powerOff);

  // Start the client (no-op when disabled). Returns false on a config error or
  // if the client could not be started.
  bool begin(const MqttHaConfig& cfg);

  // Record the latest telemetry from any task; the publisher task sends it.
  void publishState(const FoggerSnapshot& snap);

  // Connection state for the UI. Thread-safe.
  bool connected() const { return connected_.load(); }
  std::string lastError() const;  // "" when connected / no error yet

 private:
  // esp-mqtt event callback (esp_event_handler_t; base is esp_event_base_t).
  static void onEvent(void* self, const char* base, int32_t id, void* data);
  static void publisherEntry(void* self);
  void runPublisher();
  void onConnected();
  void onData(void* event);   // esp_mqtt_event_handle_t
  void onError(void* event);  // esp_mqtt_event_handle_t
  void setError(const std::string& msg);
  bool publish(const std::string& topic, const std::string& payload, bool retain);
  void publishCurrentState();
  void publishDiscovery();
  void publishDiscoveryEntity(const char* component, const char* object,
                              const std::string& configJson);
  void onMessage(const std::string& topic, const std::string& payload, bool retained);
  std::string deviceTopic(const char* leaf) const;  // base/id/<leaf>
  std::string stateJson(const FoggerSnapshot& snap) const;

  MqttHaConfig cfg_;              // const after begin(); esp-mqtt keeps a pointer
                                  // to cfg_.broker.caPem, so it must stay alive
  std::string availTopic_;        // LWT / availability topic (const after begin())
  void* client_ = nullptr;        // esp_mqtt_client_handle_t
  bool started_ = false;          // set once in begin()
  std::atomic<bool> connected_{false};
  std::atomic<bool> statePending_{false};
  uint32_t nextPublishMs_ = 0;    // publisher task only

  // Guards lastSnap_ (written by publishState() on loop) and lastError_
  // (written on the esp-mqtt task, read by the web task). SemaphoreHandle_t.
  // Atomic: created in begin() on loop while the web task may already call
  // lastError() (null => not started, nothing written yet).
  std::atomic<void*> mux_{nullptr};
  FoggerSnapshot lastSnap_;
  std::string lastError_;

  FogFn fogStart_;
  FogFn fogStop_;
  DensityFn density_;
  FanFn fan_;
  FogFn powerOff_;
};

}  // namespace fogger
