# Flashing the AZ3166 from Windows (no dev environment)

Written 2026-08-04, after a drag-and-drop flash bricked a board.

## Never drag-and-drop a .bin onto the AZ3166 drive

The board mounts as a mass-storage drive with a DAPLink-style `DETAILS.TXT`, so it
*looks* like a drag-and-drop target. It isn't.

The app is linked at **`0x0800C000`** (after the bootloader). Drag-and-drop writes to
**`0x08000000`** — on top of the bootloader. Result: blank OLED, solid red RGB, board
appears dead. This is the same symptom as a missing/mismatched bootloader, documented in
`flash-bootloader.sh`.

The board is **not** bricked when this happens. The ST-Link interface is a separate chip
and keeps enumerating; the flash just has to be rewritten at the correct addresses.

## What to flash where

| File | Address |
|---|---|
| `boot.bin` | `0x08000000` |
| `telemetry.ino.bin` | `0x0800C000` |

Copy both from the Mac:

```
~/Library/Arduino15/packages/AZ3166/hardware/stm32f4/2.0.0/bootloader/boot.bin
<repo>/firmware/telemetry/build/telemetry.ino.bin
```

`boot.bin` only needs writing once per board, or after a drag-and-drop accident has
overwritten it. It survives normal app reflashes.

## Steps (STM32CubeProgrammer)

Install **STM32CubeProgrammer** from ST. It is a GUI, needs no toolchain, and talks to the
AZ3166's onboard ST-Link/V2-1 directly.

1. Plug the board into a real USB-A port. Select **ST-LINK** in the top-right dropdown,
   then **Connect**. It should report an STM32F4 target.
   - If it refuses with an ST-LINK firmware complaint, use its
     *Firmware upgrade* button, then reconnect.
2. Left sidebar → **Erasing & Programming**.
3. **File path** → `boot.bin`. **Start address** `0x08000000`.
   Tick *Verify programming*. Untick *Run after programming*. → **Start Programming**.
4. **File path** → `telemetry.ino.bin`. **Start address** `0x0800C000`.
   Tick *Verify programming*. → **Start Programming**.
5. **Disconnect**, then press the board's reset button.

Both writes must report verification OK. A wrong address here is what caused the problem
in the first place — check the field before each Start.

## Expected result

OLED shows:

```
IoT Telemetry
WiFi connecting
```

then either the SSID it joined plus RSSI, or:

```
IoT Telemetry
WiFi FAILED
B+reset to cfg
```

Wi-Fi credentials live in EEPROM, not in the firmware. Set them by holding **B**, pressing
and releasing **reset**, connecting to the `AZ-xxxxxx` access point, and configuring at
`192.168.0.1`. A fresh board with empty EEPROM will show `WiFi FAILED` until you do this.

## Doing it from the Mac instead

If the USB-C dongle problem ever gets solved, `./flash-bootloader.sh` and
`./flash.sh telemetry/build/telemetry.ino.bin` do exactly the above via OpenOCD.
