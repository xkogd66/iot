# AZ3166 firmware

Firmware for the MXChip AZ3166 telemetry nodes: read onboard sensors and publish
JSON telemetry over MQTT to the Mosquitto broker on the k3s cluster.

## Layout

```
firmware/
├── telemetry/            # THE firmware — arduino-cli sketch (Wi-Fi + MQTT publisher)
│   ├── telemetry.ino
│   ├── portal_css.h      # mini.css for the config portal, generated from the core
│   ├── secrets.h         # compiled-in fallback config (gitignored)
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
# 1. Fallback config (once): copy the template. Wi-Fi is NOT read from here —
#    it comes from EEPROM. These values are only used until the portal sets them.
cp telemetry/secrets.h.example telemetry/secrets.h && $EDITOR telemetry/secrets.h

# 2. New board only: flash the matching bootloader (once per board)
./flash-bootloader.sh

# 3. Build + flash the app
./build.sh
./flash.sh telemetry/build/telemetry.ino.bin
```

Verify telemetry is flowing (from a machine that can reach the broker):

```sh
mosquitto_sub -h 192.168.1.211 -t 'sensors/#' -v        # on the home LAN
mosquitto_sub -h iot.ekskog.net -t 'sensors/#' -v       # from anywhere
# sensors/home/livingroom {"temp":22.9,"humidity":41.0,"pressure":1013.2,"rssi":-51}
```

`192.168.1.211` is the internal MetalLB IP and is unreachable off the home
network; `iot.ekskog.net:1883` is the same broker from outside. Set the latter
in the portal if a board moves.

Per-board config is set from the **config portal**, not a rebuild — see below.
`telemetry/secrets.h` only supplies the fallback used when nothing is stored.

## Configuring a board (B + reset)

Hold **B**, press+release **reset**, join the open `EkSkog-<room>` Wi-Fi network,
browse to **192.168.0.1**. The form sets Wi-Fi SSID/password *and* MQTT host,
port, site, room and client id, then saves and reboots. A blank password leaves
the stored one alone.

Config is stored as `host=..;port=..;site=..;room=..;id=..` in the EEPROM zone
`AZ_IOT_HUB_ZONE_IDX`; Wi-Fi goes to the same zones the SDK uses, so
`WiFi.begin()` picks it up. Anything omitted falls back to `secrets.h`. The
second boot screen shows `cfg: EEPROM` or `cfg: compiled` so you can see which
is live, and the client id is on the first — a board never connects with an
identity you can't read off the OLED.

Same settings over serial (115200) if a terminal is already open:
`cfg show`, `cfg clear`, `cfg host=iot.ekskog.net;room=kitchen`.

### ⚠️ Why the portal is built the way it is

Two SDK facts cost most of a day. Neither is obvious and both are load-bearing:

- **`__sys_setup()` is how we take B+reset.** The core's `main()`
  (`cores/arduino/system/_main_sys.cpp`) calls `__sys_setup()` **before** it
  checks `IsConfigurationMode()` (button A → serial CLI) and `IsAPMode()`
  (button B → *Microsoft's* Wi-Fi-only page). `__sys_setup()` is `WEAK` in
  `system/SystemFunc.cpp`, so the sketch overrides it, reads B itself and runs
  our portal — Microsoft's page is never reached. **Nothing triggered from
  `setup()` or `loop()` can do this**: both buttons are consumed at reset before
  the sketch runs, so any "hold a button while running" scheme is unreachable in
  practice.
- **`WiFiServer` cannot serve a soft AP.** `AZ3166WiFiServer.cpp` opens on
  `WiFiInterface()` — the *station* interface — but a phone joined to the AP
  talks to `WiFiAPInterface()`. The portal therefore uses mbed `TCPServer`
  directly: `open(WiFiAPInterface())` → `bind(80)` → `listen(1)` → `accept()`.
  Using `WiFiServer` here produces an AP that comes up and then answers nothing.
  (Also: `WiFiServer::send()` is declared in the header but never implemented —
  it fails at link time.)

AP bring-up mirrors the core's own `EnterAPMode()`: `InitSystemWiFi()` then
`SystemWiFiAPStart(ssid, "")`.

### Regenerating `portal_css.h`

It's the core's own `page_head` (mini.css v2.1.5) lifted verbatim so our portal
matches the SDK's look. Regenerate it rather than hand-editing:

```sh
CORE=~/Library/Arduino15/packages/AZ3166/hardware/stm32f4/2.0.0
SRC="$CORE/cores/arduino/httpserver/app_httpd.cpp"
LN=$(grep -n 'page_head *=' "$SRC" | head -1 | cut -d: -f1)
{ echo "#ifndef PORTAL_CSS_H"; echo "#define PORTAL_CSS_H"; echo
  printf 'static const char PORTAL_HEAD[] = '
  sed -n "${LN}p" "$SRC" | sed 's/^static const char \* page_head = //' \
                         | sed 's/AZ3166 WiFi Config/EkSkog IoT/'
  echo; echo "#endif // PORTAL_CSS_H"; } > telemetry/portal_css.h
```

## Known quirks

- **Serial over USB-CDC is flaky right after a flash** (lockup state). Read status off the
  OLED; a USB power-cycle restores clean serial.
- **Temperature reads a few °C high** — the HTS221 sits near the self-heating CPU/Wi-Fi.
  Worth a calibration offset later.
