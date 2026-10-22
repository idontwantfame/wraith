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
| `web_server` | ESPAsyncWebServer: the UI (compiled into the firmware from `data/` by `tools/web_assets.py`, served gzipped), `/api/*`, the `/ws` status push, the captive portal, signed OTA. |
| `mqtt_ha`, `mqtt_settings.h` | ESP-IDF esp-mqtt client on its own task, Home Assistant discovery, settings validation (host-tested). |
| `wifi_mgr` | Station mode with an open, setup-only access point as fallback, captive portal, mDNS. Joins the strongest access point of the network (full scan, sorted by signal); no roaming afterwards. |
| `ota`, `ota_signing.h`, `fw_marker.h`, `fw_version` | Signature-checked OTA, the embedded build number, the downgrade floor. |
| `config` | Settings in NVS behind one writer lock. |
| `timekeeper` | SNTP, POSIX time zone, optional time-zone guess from the public IP. |
| `main.cpp` | Wiring, the boot sequence, the serial console (`stop`, `x`, `status`, `poweroff`). |

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
- **Low memory must not reboot the controller.** The firmware is built with C++
  exceptions, so an uncaught `std::bad_alloc` on any task aborts the chip. On
  2026-10-22 one did: the web status push, then sent on every BLE frame, ran
  the heap out while a browser loaded the UI. Status pushes (WebSocket and
  MQTT) now go out at most once a second, and every path that builds status
  JSON catches `bad_alloc` and skips that update. A WebSocket client may queue
  at most 4 messages and is dropped when it stops reading (a dead page used to
  hold ~38 KB). `/api/status` reports `heap`, `ws_clients` and `boot`
  (reset reason, uptime).
- Power-off (web, API, serial, MQTT) is sent only while connected, never after
  a reconnect, so a machine someone just switched on is never switched straight
  back off. It ends any program first. Until the link drops (at most 15 s), fog
  start, density and fan are refused, and a program started meanwhile never
  fogs (it ends as "machine did not start"). Wraith never sends `03 00`, which
  the remote uses after the machine's own power control, so that control
  should keep working (untested).
- **If the controller dies, the machine keeps fogging for about a minute.** In
  attended tests (2026-10-21), pulling the ESP32's power mid-fog left the
  machine spraying for roughly 60 s before it stopped by itself. Nothing on the
  controller can send a stop then, so avoid anything that resets it while fog
  runs. Opening the USB console with DTR / RTS turned off resets the C3
  (tested 2026-10-22), so `pio device monitor` keeps both on, and other
  terminals must too. A normal disconnect is different: it sends a stop first.

## Security model

- The web UI and API have no login: they are meant for a trusted LAN. Every
  state-changing request needs an `X-Requested-With` header and a `Host` of the
  device IP or `<hostname>.local` (CSRF / DNS-rebinding protection). JSON routes
  are POST-only, and a strict Content-Security-Policy keeps radio-supplied names
  (BLE, SSIDs) from running as code.
- The setup access point is **open**, so onboarding needs only a phone. It is
  up only while Wraith has no working Wi-Fi: first boot or after Forget Wi-Fi
  (until set up), or, with a network saved, when it fails three times **within
  the first 10 minutes after power-on** (it closes again after that once nobody
  is on it; later failures are just retried, so power-cycle Wraith to
  reconfigure). While it is up, the server allows only `GET /api/status`,
  `GET /api/wifi/scan` and `POST /api/wifi/connect`, for every client;
  everything else (fog, power-off, MQTT, OTA, settings) is refused. Residual
  risks: the home Wi-Fi password typed during setup crosses the open network
  unencrypted, and someone nearby while it is up can point Wraith at their own
  network and then use it there.
- The **device password** authorises remote fog START over MQTT. The user
  chooses it the first time they allow remote START; it is needed again to
  change the broker / topic / TLS trust while remote START is on. A factory
  reset clears it, which is also how a forgotten one is recovered. Anyone on
  the LAN can still use the web UI (no login), so it guards the broker-wide
  remote-START channel, not the LAN.
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
| GET | `/api/status` | Machine link, fog state, batteries, machine report, program (incl. `last_stop`), Wi-Fi (incl. access point `bssid` / `channel`), MQTT, `fw_build`, `heap` (`free`, `min_free` since boot, `max_block`), `boot` (`reason`, `reason_code`, `uptime_s`), `ws_clients` |
| GET | `/api/config` | Saved settings (never returns passwords) |
| POST | `/api/fog/arm`, `/api/fog/start` | Two-step fog start |
| POST | `/api/fog/stop` | Stop fog and any program |
| POST | `/api/density` | `{"value": 8000..40000}` |
| POST | `/api/fan` | `{"value": 35..100}` |
| POST | `/api/program/start` | `{"on_sec", "off_sec", "repeats"}`; needs the arm step like a start |
| POST | `/api/program/stop` | Cancel the program and stop |
| GET | `/api/machines` | Discovered machines and the saved target |
| POST | `/api/machine/select`, `/api/machine/disconnect` | `{"mac"}` / clear the target |
| POST | `/api/machine/power_off` | Stop any program and switch the machine off (one-way: its own button switches it on) |
| GET | `/api/wifi/scan` | Cached networks (`?force=1` rescans) |
| POST | `/api/wifi/connect` | `{"ssid", "pass"}` |
| POST | `/api/wifi/forget` | Forget the saved network and restart into the setup access point; refused while fog runs |
| POST | `/api/mqtt` | Broker settings; reboots. `device_password` is needed to allow remote control (the first time, it sets it; 8+ characters). |
| POST | `/api/hostname`, `/api/time`, `/api/time/guess` | mDNS name; time zone and NTP server; time-zone guess |
| POST | `/api/ota?sig=<url-encoded base64>` | Multipart upload of `firmware.bin` |
| POST | `/api/reboot`, `/api/factory_reset` | Reboot; erase settings, device password included (keeps the OTA floor) |

Example, stop from a script:
`curl -X POST -H 'X-Requested-With: fogger' http://wraith.local/api/fog/stop`

## MQTT / Home Assistant

Topics are `<base>/<device id>/...`, where `<base>` defaults to `wraith` and the
device id is `wraith-xxxxxx` (from the chip's MAC).

| Topic | Direction | Payload |
|---|---|---|
| `.../state` | device → broker, retained | JSON: `fog`, `density`, `fan`, `batt_a`, `batt_b`, `batt_min`, `charger`, `charge_done`, `fault`, `link`, `rssi`, `connected`, `time`. Sent at most once a second when telemetry changes, and every 5 s regardless. |
| `.../status` | device → broker | Availability: `online` / `offline` |
| `.../fog/set` | broker → device | `ON` / `OFF` (`ON` only when remote control is allowed) |
| `.../density/set`, `.../fan/set` | broker → device | Raw values as above (only when remote control is allowed) |
| `.../power/set` | broker → device | `OFF` switches the machine off (always accepted, like stop) |

Home Assistant discovery (`homeassistant/...`) follows the remote-control
setting: when it is off, HA shows Stop and Power off buttons and read-only
sensors; when it is on, a fog switch and density / fan sliders appear as well.

## Storage and flash layout (don't change)

- NVS namespace `ulface`: the project's original name. Renaming it would orphan
  every saved setting on existing devices.
- `partitions_ota.csv` (4 MB): NVS, OTA data, two 1.8 MB app slots, a
  file-system partition (`spiffs`, at `0x390000`), core dump. OTA can't change
  the partition table on devices in the field, so keep it as it is.
- The web UI is part of the firmware image, so it is signed with it and
  updated with it. The file-system partition is unused (older builds served
  the UI from it).

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
- Power off from Wraith: the machine switches off within ~10 s and the link
  drops; after a power-on with its own button, Wraith reconnects and fog
  starts normally.
- The machine's own power control with Wraith connected switches it off.
- Phone onboarding through the captive portal on iOS and Android.
- MQTT over WebSocket against a real broker listener.
