# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Working rule: ask, don't dig

Don't run shell commands to find out things the user can simply tell you. Their answer
costs one line; a `find`/`grep`/`ls` sweep costs them a turn and usually lands on the wrong
file anyway.

Before reaching for Bash to locate, list, or identify something, ask. Say what you need to
know and why it matters for the task — not just "may I run X".

Ask when the question is: where does X live, which of these is current, what's the
address/name/credential, is this deployed, did you already try Y. Don't ask when the user
has already named the file or told you to run the thing — then just do it.

## What this is

Home telemetry: MXChip AZ3166 boards publish JSON over MQTT → Mosquitto (k3s) → Telegraf →
InfluxDB → a dependency-free Node dashboard. `README.md` holds the full architecture,
addresses, and operational detail; `firmware/README.md` holds the toolchain story;
`SECRETS.md` holds the secret-handling policy. Read those before changing anything —
this file only covers what they don't.

## Commands

```sh
# firmware (from firmware/)
./flash-bootloader.sh                     # ONCE per physical board, before any Wi-Fi build
./build.sh                                # arduino-cli compile of telemetry/
./flash.sh telemetry/build/telemetry.ino.bin

# dashboard (from dashboard/)
node --check server.js                    # no test suite, no deps — this is the check
INFLUX_URL=http://192.168.1.216:8086 INFLUX_TOKEN=... INFLUX_ORG=iot INFLUX_BUCKET=iot-data \
  PORT=8099 node server.js

# cluster (everything except dashboard/ is applied by hand)
kubectl apply -f mqtt/ -f influxdb/ -f telegraf/ -f grafana/
```

There are no tests, no linter, and no package manager step anywhere in this repo.

## Things that will bite you

- **Only `dashboard/**` is deployed by CI** (`.github/workflows/dashboard.yml` → GHCR →
  `kubectl apply`). Editing any other manifest changes nothing until someone applies it.
- **`firmware/telemetry/` is the firmware.** `firmware/src/` + `platformio.ini` is an older
  sensor-only PlatformIO project whose Wi-Fi path was never verified — don't build from it.
- **Board identity is compiled in.** `SITE`, `ROOM`, `MQTT_CLIENT_ID` come from the
  gitignored `telemetry/secrets.h`, so that file describes the *last board flashed*. Two
  boards sharing a client id kick each other off the broker in a loop.
- **The dashboard has zero npm dependencies on purpose** — Node stdlib + global `fetch`
  only, so the Dockerfile needs no install step. Don't add one.
- **InfluxDB's `strategy: Recreate` is load-bearing** (RWX NFS PVC; two pods would corrupt
  the bolt/TSM files).
- **Never drag-and-drop a `.bin` onto the AZ3166 drive, and never tell the user to.**
  Sketches link at `0x0800C000` (`AZ3166.ld`, and `platform.txt`'s upload recipe programs
  that address); the drive writes at `0x08000000`, landing the app on top of the bootloader
  → blank OLED, solid red RGB, board looks dead. Only `0x08000000`-based images (Microsoft's
  release firmware, `boot.bin`) are drop-safe. Verify by checking a `.bin`'s second word —
  the reset vector — before dropping anything. Use `flash.sh`/OpenOCD, or STM32CubeProgrammer
  with explicit addresses; recovery is in `firmware/FLASH-WINDOWS.md`. This has cost a board
  twice; **read `flash.sh` and `flash-bootloader.sh` before giving any flashing advice.**
- **Wi-Fi credentials come from EEPROM, never compiled in.** `connectWiFi()` calls the no-arg
  `WiFi.begin()`, which reads what the config-mode AP page wrote (hold B, press+release
  reset). Don't reintroduce `WIFI_SSID`/`WIFI_PASSWORD` from `secrets.h` or a "fallback if
  EEPROM is blank" path — both were explicitly declined.
- **Never commit a `secret.yaml`** — `.gitignore` blocks them; they're applied out-of-band
  from Proton Pass. See `SECRETS.md`.

## Dashboard query safety

`server.js` interpolates values into Flux strings. Two guards do all the work, and any new
query must keep them: `site` is regex-checked *and* membership-checked against live
`listSites()` before use, and `range` is an allowlist key (`RANGES`) mapped to a fixed Flux
literal. Never interpolate a raw query-string value.

Adding a site or room requires no config change anywhere — Telegraf's `topic_parsing` turns
the `sensors/<site>/<room>` path into tags, and the dashboard derives its dropdown from
`schema.tagValues`.

## README drift

`README.md`'s dashboard route table is stale: the live routes are `/api/status` (all sites +
lastSeen) and `/api/summary?site=X&range=6h|24h|7d` (1h averages + max/min with timestamps),
not `/api/sites`. `grafana/` also exists but isn't in the README's layout section. Fix these
in passing if you touch them.

## Physical-world caveats

Sensor readings need calibration, not code purity: temperature reads a few °C high (HTS221
next to the self-heating CPU), and pressure swings for minutes after a reflash. Leave room
for offsets rather than assuming the numbers are ground truth.
