# Architecture, safety and interfaces

Wraith runs on an ESP32-C3. It holds a BLE link to the fog machine (see
[PROTOCOL.md](PROTOCOL.md)) and serves a web UI, a REST / WebSocket API and an
optional MQTT / Home Assistant integration over Wi-Fi.

```
            +--------------------- ESP32-C3 ---------------------+
  BLE  <----| fogger_ble <-> sequencer / app state <-> web_server |----> browser
 (machine)  |      ^                    |                ^         |     (Wi-Fi)
            | fogger_protocol        mqtt_ha ------------+-------> MQTT broker
            |                 config (NVS)   wifi_mgr   ota       |
            +----------------------------------------------------+
```

## Modules (`src/`, `include/`)

| Module | Role |
|---|---|
| `fogger_protocol.h` | Wire protocol: command builders, status parser, stream reassembler. No Arduino dependencies, so it is host-tested. |
| `fogger_ble` | NimBLE central: scan, connect / reconnect, commands queued and written from `loop()`, a latched STOP; reads the live link's signal every 2 s. |
| `sequencer` | Timed programs (on / off / repeat) with safety caps. |
| `web_server` | ESPAsyncWebServer: the UI from LittleFS, `/api/*`, the `/ws` status push, the captive portal, signed OTA. |
| `mqtt_ha`, `mqtt_settings.h` | ESP-IDF esp-mqtt client on its own task, Home Assistant discovery, settings validation (host-tested). |
| `wifi_mgr` | Station mode with a WPA2 setup access point as fallback, captive portal, mDNS. Joins the strongest access point of the network (full scan, sorted by signal); no roaming afterwards. |
| `ota`, `ota_signing.h`, `fw_marker.h`, `fw_version` | Signature-checked OTA, the embedded build number, the downgrade floor. |
| `config` | Settings in NVS behind one writer lock. |
| `timekeeper` | SNTP, POSIX time zone, optional time-zone guess from the public IP. |
| `main.cpp` | Wiring, the boot sequence, the serial console (`stop`, `x`, `status`). |

Hardware-facing work (BLE writes, Wi-Fi changes) happens only on the `loop()`
task; other tasks hand requests over through mutex-guarded or atomic state.

## Safety rules (don't regress)

- Fog never starts by itself: not on boot, not on reconnect, not from a
  retained MQTT message.
- **Stop always works**, from every interface, and is latched: it is retried
  until the write succeeds and survives a link drop, so a stop requested while
  disconnected is sent on reconnect. A stop also goes out before every reboot,
  OTA and factory reset, and to the old machine before switching to another.
  If the link drops while fogging, a stop is sent as soon as it reconnects.
- Starting is two-step on the server: `POST /api/fog/arm`, then
  `POST /api/fog/start` within 8 s. The UI asks for confirmation.
- Programs: at most 60 s per pulse and a 2-hour total runtime; a manual or MQTT
  command cancels a running program. The serial `stop` cancels programs too.
- **A program never restarts fog the machine stopped.** During each pulse it
  watches the machine's output byte (`program_watch.h`): if output stops early
  (the machine's button, its protection) or never starts, the program ends with
  a stop, and `/api/status` says why (`program.last_stop`). Limit: the machine
  needs up to ~0.65 s to switch, so a stop in roughly the first second of a
  pulse, or just before its scheduled end, looks like the program's own; then
  the next pulse still starts. Short-pulse programs (e.g. 1 s strobe) should be
  stopped from Wraith.
- Remote control over MQTT (start, density, fan) is off by default. Turning it
  on needs the device password. Stop is always accepted.
- OTA is refused while fog runs or a stop is still pending.
- **If the controller dies, the machine keeps fogging for about a minute.** In
  attended tests (2026-10-21), pulling the ESP32's power mid-fog left the
  machine spraying for roughly 60 s before it stopped by itself. Nothing on the
  controller can send a stop then, so avoid anything that resets it while fog
  runs; the serial monitor opens without toggling DTR / RTS for this reason.
  A normal disconnect is different: it sends a stop first.

## Security model

- The web UI and API have no login: they are meant for a trusted LAN. Every
  state-changing request needs an `X-Requested-With` header and a `Host` of the
  device IP or `<hostname>.local` (CSRF / DNS-rebinding protection). JSON routes
  are POST-only, and a strict Content-Security-Policy keeps radio-supplied names
  (BLE, SSIDs) from running as code.
- The setup access point is WPA2 with a per-device password, generated on
  first boot and printed on the serial console at every boot. It survives a
  factory reset and doubles as the **device password**.
- OTA images must be signed (RSA-2048 / SHA-256); the signature is checked
  before the new image is made bootable. Each image carries a build number and
  the device refuses anything older than the newest build it has run.
- MQTT: retained commands are ignored, and a retained `fog/set` is cleared on
  connect. TLS (`mqtts`, `wss`) is compiled out on the ESP32-C3 for lack of RAM
  (`WRAITH_MQTT_TLS=0`).
- Not done: flash encryption and secure boot. Both need eFuse provisioning.

## HTTP API

State-changing requests need an `X-Requested-With` header (any value) and take
JSON bodies. `/ws` pushes the same JSON as `/api/status` about once a second.

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/status` | Machine link, fog state, batteries, machine report, program (incl. `last_stop`), Wi-Fi (incl. access point `bssid` / `channel`), MQTT, `fw_build` |
| GET | `/api/config` | Saved settings (never returns passwords) |
| POST | `/api/fog/arm`, `/api/fog/start` | Two-step fog start |
| POST | `/api/fog/stop` | Stop fog and any program |
| POST | `/api/density` | `{"value": 8000..40000}` |
| POST | `/api/fan` | `{"value": 35..100}` |
| POST | `/api/program/start` | `{"on_sec", "off_sec", "repeats"}`; needs the arm step like a start |
| POST | `/api/program/stop` | Cancel the program and stop |
| GET | `/api/machines` | Discovered machines and the saved target |
| POST | `/api/machine/select`, `/api/machine/disconnect` | `{"mac"}` / clear the target |
| GET | `/api/wifi/scan` | Cached networks (`?force=1` rescans) |
| POST | `/api/wifi/connect` | `{"ssid", "pass"}` |
| POST | `/api/mqtt` | Broker settings; reboots. `device_password` is needed to allow remote control. |
| POST | `/api/hostname`, `/api/time`, `/api/time/guess` | mDNS name; time zone and NTP server; time-zone guess |
| POST | `/api/ota?sig=<url-encoded base64>` | Multipart upload of `firmware.bin` |
| POST | `/api/reboot`, `/api/factory_reset` | Reboot; erase settings (keeps the device password and the OTA floor) |

Example, stop from a script:
`curl -X POST -H 'X-Requested-With: fogger' http://wraith.local/api/fog/stop`

## MQTT / Home Assistant

Topics are `<base>/<device id>/...`, where `<base>` defaults to `wraith` and the
device id is `wraith-xxxxxx` (from the chip's MAC).

| Topic | Direction | Payload |
|---|---|---|
| `.../state` | device → broker, retained | JSON: `fog`, `density`, `fan`, `batt_a`, `batt_b`, `batt_min`, `charger`, `charge_done`, `fault`, `link`, `rssi`, `connected`, `time` |
| `.../status` | device → broker | Availability: `online` / `offline` |
| `.../fog/set` | broker → device | `ON` / `OFF` (`ON` only when remote control is allowed) |
| `.../density/set`, `.../fan/set` | broker → device | Raw values as above (only when remote control is allowed) |

Home Assistant discovery (`homeassistant/...`) follows the remote-control
setting: when it is off, HA shows a Stop button and read-only sensors; when it
is on, a fog switch and density / fan sliders appear as well.

## Storage and flash layout (don't change)

- NVS namespace `ulface`: the project's original name. Renaming it would orphan
  every saved setting on existing devices.
- `partitions_ota.csv` (4 MB): NVS, OTA data, two 1.8 MB app slots, LittleFS
  (must stay labelled `spiffs`, at `0x390000`), core dump.
- The web UI lives on LittleFS and changes only by USB (`pio run -t uploadfs`)
  or a factory image; OTA replaces the app alone.

## Hardware checks

Passed in attended tests (2026-10-20/21, build 2610151253; evidence in the
ULFACE repo, `docs/HANDOFF_2026-10-21.md`): 30 s and 90 s fog runs; normal
Disconnect while fogging (stop sent first); web / API stop during a program
(later pulses cancelled); removing the machine's batteries mid-program (program
aborted, stop on reconnect, no restart).

Still to verify, with the machine on and someone watching it:

- The machine's own button during a program now ends the program (fixed after
  the 2026-10-21 test showed the next pulse restarting fog).
- Serial `stop` and `status` over USB (the console moved to the native USB
  port; before, it listened on UART0 pins and never answered over USB).
- Phone onboarding through the captive portal on iOS and Android.
- MQTT over WebSocket against a real broker listener.
