# Wraith

Firmware for an ESP32-C3 that controls an **Ulanzi Filmog Ace (FM01)** fog
machine over Bluetooth LE. It gives the machine a web UI and an API on your
Wi-Fi, timed fog programs, and an optional Home Assistant integration over MQTT.

> **Safety.** This drives a real fog machine. Fog never starts on its own; a
> start needs a confirmed, two-step request, and stop is always one tap and
> always accepted. Keep fog runs attended, and keep the device on a trusted
> network: the web UI has no login. If the controller loses power or resets
> while fogging, the machine keeps going for about a minute before it stops by
> itself; use the machine's own button to stop it sooner.

## Features

- Start / stop fog, set density and fan, see both battery packs, the charger
  and fault status live.
- Timed programs: fog on X seconds, off Y, repeated N times (or until stopped),
  with a 60 s per-pulse and 2-hour total cap.
- Finds machines nearby and remembers the one you pick; reconnects on its own.
- Powers the machine off, as its remote does. Switching it back on needs the
  machine's own button.
- Signal bars for Wi-Fi and the machine link in the top bar (dBm on hover).
- Wi-Fi setup through a captive portal on its own WPA2 access point.
- Home Assistant discovery over MQTT. Remote start is off unless you allow it.
- Signed over-the-air updates with downgrade protection.

## What you need

- An ESP32-C3 board with 4 MB flash whose USB connector is the chip's native
  USB (most small C3 boards). The serial console uses it. For a board with a
  USB-UART bridge chip instead, set `ARDUINO_USB_MODE` and
  `ARDUINO_USB_CDC_ON_BOOT` to 0 in `platformio.ini`.
- Serial console in another terminal (not `pio device monitor`): leave DTR
  and RTS on. Turning them off resets the board when the port opens, and a
  reset while fogging leaves the machine spraying for about a minute.
- The fog machine. It shows up as `CH9141BLE2U`. Switch its handheld remote
  off while Wraith is in charge: the remote reconnects and takes the link.

## Install

### From a release

Each [release](../../releases) has a complete image. Flash it over USB with
[esptool](https://docs.espressif.com/projects/esptool/) (`pip install esptool`):

```
esptool --chip esp32c3 write-flash 0x0 wraith-<version>-factory.bin
```

This erases any saved settings. To update a running device, use the Wi-Fi
update below instead.

### From source

Install [PlatformIO Core](https://platformio.org/install/cli) (or the
PlatformIO IDE extension for VS Code), then from the repo root:

```
pio run -t upload       # build and flash the firmware (web UI included) over USB
```

## First setup

1. Power the board. With no Wi-Fi saved, it opens an access point named
   `wraith-xxxxxx`. Its password is generated on first boot and printed on the
   serial console at every boot: `pio device monitor` (115200 baud) shows
   `setup AP: SSID '...' password '...'`. Keep it: it is also the **device
   password**, needed to allow remote control later.
2. Join the access point. The setup page opens (or browse to `http://192.168.4.1`).
   Pick your network and enter its password.
3. Open `http://wraith-xxxxxx.local` (or the IP from your router). In
   **Settings → Fog machine**, pick your machine.

## Using it

- **Control**: start (asks to confirm), stop, density, fan, and timed programs.
  The machine's last report shows as a banner above; × hides it until the
  machine reports something else.
- **Settings**: Wi-Fi (connect, or forget it to rerun setup), fog machine
  (pick, disconnect, power off), Home
  Assistant, device name (sets `http://<name>.local`), time zone, firmware
  update, reboot and factory reset.
- **Home Assistant**: in Settings, enter your MQTT broker. Entities appear by
  MQTT discovery: Stop and Power off buttons and read-only sensors by default.
  "Allow remote fog START + density/fan" (needs the device password) adds a fog
  switch and sliders, for automations you trust. Power off needs no permission:
  anything that can publish to the broker can switch the machine off.
- **Scripts**: see the HTTP API in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#http-api).
  For example, to stop:
  `curl -X POST -H 'X-Requested-With: fogger' http://wraith.local/api/fog/stop`

## Updating over Wi-Fi

The device only installs firmware signed with the project's key, and refuses
an older build than it has run. The web UI is built into the firmware, so an
update brings both.

- **A release**: in Settings → Firmware update, pick
  `wraith-<version>-firmware.bin` and its `.sig`.
- **From source**: `pio run -t ota` builds, signs and uploads to `wraith.local`
  (set `custom_ota_host` in `platformio.ini`, or run with
  `WRAITH_HOST=<ip or name.local>`). Signing needs the private key at
  `tools/ota/ota_private_key.pem`; see [Signing](#signing).

## Development

```
pio run                            # build (stamps the build number, signs if the key is present)
pio run -t upload                  # flash the firmware (web UI included) over USB
pio run -t ota                     # build, sign and install over Wi-Fi
pio test -e native -e native_tls   # host unit tests
pio device monitor                 # serial console: logs, `stop`, `status`, `poweroff`
```

- **Build numbers** are automatic: the UTC build time as `yymmddHHMM` (e.g.
  `2610151253`), reported as `fw_build` by `/api/status`. To rebuild a specific
  one: `WRAITH_FW_BUILD=2610151253 pio run`.
- **Versions are pinned**: the platform to a pioarduino release tag (Arduino
  core 3.3.12), the libraries to patch releases (`~X.Y.Z`) in `platformio.ini`.
- **Layout**:

  | Path | What |
  |---|---|
  | `src/`, `include/` | Firmware (modules are described in [ARCHITECTURE.md](docs/ARCHITECTURE.md)) |
  | `data/` | Web UI; compiled into the firmware, gzipped, at build time |
  | `test/` | Host unit tests (protocol, MQTT settings, program watch) |
  | `tools/` | Build-number stamping, web UI embedding, OTA signing and upload, release packaging |
  | `docs/` | [The BLE protocol](docs/PROTOCOL.md) and [architecture, safety and interfaces](docs/ARCHITECTURE.md) |

### Signing

`tools/ota/ota_private_key.pem` (never committed) signs every build; its public
half is compiled into `src/ota_signing.h` and devices trust only that key.
Without the file you can still build and flash over USB; signing is skipped.

- **Don't** run `tools/ota/gen_keys.sh` for this project: a new key means
  re-flashing every device over USB with the new public key.
- Keep an offline backup of the key; it is also the `OTA_SIGNING_KEY` secret
  that signs releases.
- `-DOTA_REQUIRE_SIGNATURE=0` in `build_flags` builds firmware that accepts
  unsigned updates. Bench use only: anyone on the network could then install
  firmware that runs the fog machine.

## Releases

Pushing a tag `vX.Y.Z` (`vX.Y.Z-rc.N` for a pre-release) on a commit in
`main` runs the [release workflow](.github/workflows/release.yml). Tag with a
message: `git tag -a v1.0.0 -m v1.0.0 && git push origin v1.0.0`.

1. Runs the tests and builds the firmware (web UI included) from fresh
   downloads (no cache, no secrets).
2. Packages the factory and firmware images and the public key, and
   writes the release notes: install instructions plus a changelog from the
   commit messages since the previous tag ([git-cliff](https://git-cliff.org),
   `cliff.toml`).
3. In a separate job that alone can read the key, checks that the key matches
   `src/ota_signing.h` and that the firmware embeds it, signs the firmware and a
   `SHA256SUMS` file, verifies both, and publishes the GitHub release.

One-time setup on GitHub, so that only release tags can reach the key:

1. Settings → Environments → **New environment** `release`. Under deployment
   branches and tags, allow only tags matching `v*`. Optionally add yourself as
   a required reviewer, so every release waits for your approval.
2. In that environment, add the secret `OTA_SIGNING_KEY` with the contents of
   `tools/ota/ota_private_key.pem` (or
   `gh secret set OTA_SIGNING_KEY --env release < tools/ota/ota_private_key.pem`).
   Make it an environment secret, not a repository secret: a repository secret
   is readable from any branch.
3. Settings → Rules → Rulesets: a tag ruleset for `v*` that only you can
   create, update or delete.

Running the workflow by hand is a dry run: it builds and packages without
publishing. Actions are pinned to commit SHAs; Dependabot proposes updates.

Commit messages follow [Conventional Commits](https://www.conventionalcommits.org)
(`feat:`, `fix:`, `docs:`, `build:` ...), which is what the changelog groups by.
Every push and pull request also runs [CI](.github/workflows/ci.yml): tests and
an unsigned build.
