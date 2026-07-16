# AZ3166 firmware

Firmware for the MXChip AZ3166 telemetry nodes: read onboard sensors and publish
JSON telemetry over MQTT to the Mosquitto broker on the k3s cluster.

## Layout

```
firmware/
├── telemetry/            # THE firmware — arduino-cli sketch (Wi-Fi + MQTT publisher)
│   ├── telemetry.ino
│   ├── secrets.h         # your Wi-Fi/broker config (gitignored)
│   └── secrets.h.example # template
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

## Flashing — the board is an ST-Link (not DAPLink)

The onboard probe enumerates as a real ST-Link/V2-1 (USB 0x0483:0x374B). Stock `pio upload`
fails (`hla_swd` unsupported), and mass-storage drag-drop corrupts (wrong offset). `flash.sh`
drives OpenOCD with `dapdirect_swd`, programming the app at `0x0800C000`.

## Full workflow

```sh
# 1. Configure (once): copy the template and fill in Wi-Fi + broker
cp telemetry/secrets.h.example telemetry/secrets.h && $EDITOR telemetry/secrets.h

# 2. New board only: flash the matching bootloader (once per board)
./flash-bootloader.sh

# 3. Build + flash the app
./build.sh
./flash.sh telemetry/build/telemetry.ino.bin
```

Verify telemetry is flowing (from a machine that can reach the broker):

```sh
mosquitto_sub -h 192.168.1.211 -t 'sensors/#' -v
# sensors/home/livingroom {"temp":22.9,"humidity":41.0,"pressure":1013.2,"rssi":-51}
```

Per-board config (site/room/client-id/topic) lives in `telemetry/secrets.h`.

## Known quirks

- **Serial over USB-CDC is flaky right after a flash** (lockup state). Read status off the
  OLED; a USB power-cycle restores clean serial.
- **Temperature reads a few °C high** — the HTS221 sits near the self-heating CPU/Wi-Fi.
  Worth a calibration offset later.
