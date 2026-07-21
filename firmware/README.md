# AZ3166 firmware

Firmware for the MXChip AZ3166 telemetry nodes: read onboard sensors and publish
JSON telemetry over MQTT to the Mosquitto broker on the k3s cluster.

## Layout

```
firmware/
├── telemetry/            # THE firmware — arduino-cli sketch (Wi-Fi + MQTT publisher)
│   ├── telemetry.ino
│   ├── device_config.h/.cpp   # config struct + STSAFE-backed load/save
│   └── config_portal.h/.cpp   # SoftAP + web form setup mode
├── build.sh              # compile telemetry/ with arduino-cli (GCC 5.4)
├── flash-bootloader.sh   # ONE-TIME per board: flash the matching bootloader
├── flash.sh              # flash an app .bin to 0x0800C000 via ST-Link
├── platformio.ini, src/  # older PlatformIO project — sensor-only (Milestone 1).
│                         #   Do NOT use for Wi-Fi (its GCC 6.3 build path is unverified).
```

## Toolchains — why two, and which to use

- **arduino-cli + AZ3166 core (GCC 5.4)** — the working path for anything with Wi-Fi.
- **PlatformIO (GCC 6.3)** — proved the sensors/OLED (Milestone 1) but its Wi-Fi was never
  verified; use `telemetry/` via arduino-cli instead.

Install the arduino-cli core once:

```sh
brew install arduino-cli
arduino-cli config add board_manager.additional_urls \
  https://raw.githubusercontent.com/VSChina/azureiotdevkit_tools/master/package_azureboard_index.json
arduino-cli core update-index
arduino-cli core install AZ3166:stm32f4
```

> Note: on modern arduino-cli the core install fails on a missing GCC checksum in
> Microsoft's old index. If so, the tools still download to
> `~/Library/Arduino15/staging/packages/`; extract them by hand into
> `~/Library/Arduino15/packages/AZ3166/{hardware/stm32f4/2.0.0, tools/...}`.

## ⚠️ The bootloader gotcha (read this)

The AZ3166 Wi-Fi stack **crashes at boot** (blank OLED, solid-red RGB) unless the board has
the bootloader that matches SDK 2.0.0. The app's RAM starts at `0x200001C4` — the first 452
bytes are a bootloader↔app shared region, and a stock/old bootloader mismatches it and
corrupts memory during Wi-Fi init. **Neither upload recipe flashes the bootloader.**

Fix, **once per physical board**:

```sh
./flash-bootloader.sh
```

It persists across app reflashes. Sensor-only firmware works without it; Wi-Fi needs it.

## Device setup (Wi-Fi/MQTT provisioning)

There's no per-board `secrets.h` — every board runs the identical binary. Identity (Wi-Fi
SSID/password, MQTT broker host/port, site, room) is set over Wi-Fi and persisted on the
STSAFE-A100 secure element via `EEPROMInterface` (zone 7 — see `device_config.h`), not
internal flash, so it can't collide with the app image or bootloader.

- A board with no saved config **boots straight into setup mode** — no button needed.
- To reconfigure an already-provisioned board, hold **Button B** through a reset.

In setup mode the board opens an open (no-password) Wi-Fi network named `AZ3166-setup` and
its OLED shows the IP to browse to. The page there is a plain form (Wi-Fi SSID/password,
MQTT host/port, site, room); submitting it saves the values and reboots into normal
telemetry mode. `MQTT_CLIENT_ID` is derived from site+room (`az3166-<site>-<room>`) rather
than typed in, so two boards can't collide on a hand-picked client ID the way the old
`secrets.h`-based setup allowed. Setup mode also times out and reboots on its own after 10
minutes idle, in case it auto-entered with nobody around to configure it.

This mirrors (for this project's MQTT/site/room schema, not Azure IoT Hub) the config mode
Microsoft's original DevKit firmware had — see `microsoft/devkit-sdk`'s
`AZ3166WiFi`/`EEPROMInterface`/`WiFiServer` for the underlying primitives this reuses.

## Flashing — the board is an ST-Link (not DAPLink)

The onboard probe enumerates as a real ST-Link/V2-1 (USB 0x0483:0x374B). Stock `pio upload`
fails (`hla_swd` unsupported), and mass-storage drag-drop corrupts (wrong offset). `flash.sh`
drives OpenOCD with `dapdirect_swd`, programming the app at `0x0800C000`.

## Full workflow

```sh
# 1. New board only: flash the matching bootloader (once per board)
./flash-bootloader.sh

# 2. Build + flash the app (same binary for every board)
./build.sh
./flash.sh telemetry/build/telemetry.ino.bin

# 3. Provision: join the board's "AZ3166-setup" Wi-Fi network, browse to the IP
#    shown on its OLED, and fill in Wi-Fi/broker/site/room. It reboots on save.
```

Verify telemetry is flowing (from a machine that can reach the broker):

```sh
mosquitto_sub -h 192.168.1.211 -t 'sensors/#' -v
# sensors/home/livingroom {"temp":22.9,"humidity":41.0,"pressure":1013.2,"rssi":-51}
```

Per-board config (site/room/topic) lives on the board itself (see "Device setup" above),
not in a file in this repo. Hold Button B through a reset to change it later.

## Known quirks

- **Serial over USB-CDC is flaky right after a flash** (lockup state). Read status off the
  OLED; a USB power-cycle restores clean serial.
- **Temperature reads a few °C high** — the HTS221 sits near the self-heating CPU/Wi-Fi.
  Worth a calibration offset later.
