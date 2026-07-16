#!/usr/bin/env bash
# Flash the built firmware to the MXChip AZ3166 via its onboard ST-Link.
#
# Why this script exists (hard-won, 2026-07-16):
#   * The AZ3166's onboard debugger enumerates as a genuine ST-Link/V2-1
#     (USB 0x0483:0x374B, "STM32 STLink") -- NOT a CMSIS-DAP/DAPLink, despite
#     the DAPLink-style DETAILS.TXT on its mass-storage drive.
#   * `pio run -t upload` fails two ways out of the box:
#       - default protocol `stlink` forces `transport select hla_swd`, which
#         this OpenOCD build rejects ("doesn't support 'hla_swd' transport").
#       - `cmsis-dap` can't find the probe (it isn't CMSIS-DAP).
#   * Mass-storage drag-drop (cp firmware.bin /Volumes/AZ3166) does NOT work:
#     PlatformIO links the app at 0x0800C000 (after the bootloader), but the
#     drag-drop lands it at 0x08000000 -> halt -> blank OLED.
#
#   The fix: drive OpenOCD directly with the ST-Link interface and let it pick
#   its own transport (dapdirect_swd), programming at 0x0800C000.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${1:-$HERE/.pio/build/mxchip_az3166/firmware.bin}"
OCD="$HOME/.platformio/packages/tool-openocd"
FLASH_ADDR="0x0800C000"

if [ ! -f "$BIN" ]; then
    echo "Firmware not found: $BIN" >&2
    echo "Build it first:  pio run" >&2
    exit 1
fi

echo "Flashing $BIN -> $FLASH_ADDR via ST-Link ..."
"$OCD/bin/openocd" -s "$OCD/openocd/scripts" \
    -f interface/stlink.cfg \
    -f target/stm32f4x.cfg \
    -c "program {$BIN} $FLASH_ADDR verify reset; shutdown"
