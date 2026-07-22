# esp-otp 

A hardware TOTP token built on an **ESP32-C3 super mini** with a small OLED
screen, for quick access to your TOTP codes.

When connected to power, the device synchronises its time over USB and/or BLE
with a companion app (or web page). The screen shows the first TOTP code (a
description plus the 6 digits). A single button press shows the next code; a
**long press types the current code over BLE as if it were a keyboard**.

Several hosts can be connected at once (e.g. a phone running the sync app *and*
a laptop receiving keystrokes). Keystrokes go to whichever host subscribed as a
keyboard; if more than one has, the **home screen shows the active target (and a
`+`) and a double-click *there* cycles between them**.

An optional DS3231 RTC module lets codes appear immediately on power-up
without needing a host to sync first — the firmware auto-detects it and runs
fine with or without it.

Codes are managed (add / remove / rename) from an Android app or a web page
over BLE, and over USB-serial when plugged in.

## Hardware note: typing is BLE-only

The ESP32-C3's USB is a fixed USB-Serial/JTAG controller, **not** USB-OTG, so
it cannot emulate a USB HID keyboard. Keystroke injection (double-click to
type a code) therefore works over **BLE HID only**. USB remains available for
logs, time sync and the management protocol (USB-serial / Web Serial), just
not for typing keystrokes. (Full USB-keyboard support would require an
ESP32-S2/S3.)

## Building

The firmware builds inside a pinned ESP-IDF Docker image — no local toolchain
needed:

```sh
scripts/build.sh                  # build
scripts/build.sh -p /dev/ttyACM0 flash monitor
```

See the implementation plan for architecture and the protocol specification
(`PROTOCOL.md`) for the BLE/USB wire format.

## Testing (QEMU)

On-target unit tests run in the Espressif QEMU emulator bundled in the build
image — no hardware required:

```sh
scripts/test-qemu.sh    # builds test/ and runs the Unity suites in QEMU
```

This emulates the ESP32-C3 CPU and runs the real firmware, covering the TOTP
engine (RFC 6238 vectors), base32 decoding and the protocol framing. QEMU does
**not** emulate the I2C SSD1306, so read the OLED live from hardware instead
(see below).

## Reading the live screen

To see exactly what the OLED shows on a running device, fetch its framebuffer
over the wire with the `GET_SCREEN` command (see `PROTOCOL.md`):

```sh
tools/host_cli.py -p /dev/ttyACM0 screen         # ASCII rendering of the panel
tools/host_cli.py -p /dev/ttyACM0 screen --raw   # raw framebuffer bytes
```

It pulls the same `canvas.c` framebuffer the panel is driven from, so it is
pixel-accurate. Over BLE the command needs an authorized (bonded) link, since
the panel can be showing a live code or a pairing passkey.

The screen font is [Lexis](https://github.com/damianvila/font-lexis) (a 6x8
pixel font, CC0); `tools/gen_font.py` samples it into `components/display/
font6x8.h`.

## License

[MIT](LICENSE) © esp-otp contributors.
