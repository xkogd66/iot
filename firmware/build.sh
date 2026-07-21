#!/usr/bin/env bash
# Build the telemetry firmware with arduino-cli.
#
# We use arduino-cli (Microsoft AZ3166 core, GCC 5.4) rather than PlatformIO for
# anything using Wi-Fi. PlatformIO builds the same SDK with GCC 6.3; that was a
# red herring for the Wi-Fi crash (the real fix was the bootloader — see
# flash-bootloader.sh), but arduino-cli/GCC 5.4 is the proven-working path.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SKETCH="${1:-$HERE/telemetry}"
FQBN="AZ3166:stm32f4:MXCHIP_AZ3166"

arduino-cli compile --fqbn "$FQBN" "$SKETCH" --output-dir "$SKETCH/build"
echo
echo "Built: $SKETCH/build/$(basename "$SKETCH").ino.bin"
echo "Flash it with:  ./flash.sh $SKETCH/build/$(basename "$SKETCH").ino.bin"
