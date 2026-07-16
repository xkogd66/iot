#!/usr/bin/env bash
# ONE-TIME per physical board: flash the AZ3166 bootloader that matches SDK 2.0.0.
#
# Why (the single biggest gotcha of this project):
#   The app links at 0x0800C000 and its RAM starts at 0x200001C4 — the first 452
#   bytes of RAM are a bootloader<->app shared region. A stock/old bootloader
#   mismatches SDK 2.0.0's app there, corrupting memory and crashing at boot
#   inside the Wi-Fi init (blank OLED, solid-red RGB). Neither Microsoft's upload
#   recipe nor flash.sh writes the bootloader — it's assumed present.
#
#   Flashing this bootloader once fixes Wi-Fi permanently (it persists across app
#   reflashes). Sensor-only firmware works without it; anything using Wi-Fi needs it.
#
# Requires the AZ3166 arduino-cli core installed (provides boot.bin). See README.
set -euo pipefail

OCD="$HOME/.platformio/packages/tool-openocd"
BOOT=$(ls "$HOME/Library/Arduino15/packages/AZ3166/hardware/stm32f4/"*/bootloader/boot.bin 2>/dev/null | sort | tail -1)

if [ -z "${BOOT:-}" ] || [ ! -f "$BOOT" ]; then
    echo "boot.bin not found. Install the AZ3166 core first:" >&2
    echo "  arduino-cli core install AZ3166:stm32f4" >&2
    exit 1
fi

echo "Flashing bootloader $BOOT -> 0x08000000 via ST-Link ..."
"$OCD/bin/openocd" -s "$OCD/openocd/scripts" \
    -f interface/stlink.cfg -f target/stm32f4x.cfg \
    -c "program {$BOOT} 0x08000000 verify reset; shutdown"
echo "Bootloader flashed. Now build + flash the app: ./build.sh && ./flash.sh telemetry/build/telemetry.ino.bin"
