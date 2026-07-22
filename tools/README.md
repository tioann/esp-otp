# esp-otp host tools

Python helpers for talking to an esp-otp device and managing its secrets.

## Setup

The scripts use a local virtualenv at `tools/.venv`:

```sh
python3 -m venv tools/.venv
tools/.venv/bin/pip install -r tools/requirements.txt   # pyserial for host_cli.py
```

`host_cli.py` needs `pyserial` (and `platformdirs` for the shared config path,
`bleak` for `--ble`); `--selftest` runs with no dependencies and no device.
(`ble_clock_sync.py` needs `bleak` — see [clock-sync/](clock-sync/).)

## host_cli.py — manage secrets over USB or BLE

Reference client for the framed wire protocol (see [../PROTOCOL.md](../PROTOCOL.md)).
Two transports: the C3's USB-Serial/JTAG (`-p /dev/ttyACM0`) or the BLE
management GATT service (`--ble`, needs `bleak`).

```sh
CLI="tools/.venv/bin/python tools/host_cli.py -p /dev/ttyACM0"
$CLI info
$CLI settime                                  # push host time
$CLI list
$CLI add "GitHub" JBSWY3DPEHPK3PXP            # label + base32 secret
$CLI add "otpauth://totp/GitHub:me?secret=JBSWY3DPEHPK3PXP&issuer=GitHub"
$CLI move 3 0                                  # reorder: token id 3 to the top
$CLI remove 1
$CLI setname "Tasos-otp"                       # rename the BLE device (1..20 chars)
```

`setname` changes the advertised BLE device name (persisted on the device), so
several people sharing one device can tell theirs apart; `info` reports the
current name. The name is cosmetic — tools identify the device by its management
service UUID, so a rename never breaks discovery (see below).

Over BLE, swap `-p /dev/ttyACM0` for `--ble`. It finds any esp-otp by its
advertised management **service UUID** (rename-proof — not the changeable GAP
name); narrow with `--name "Tasos-otp"` when several are in range, or pin an
exact device with `--address AA:BB:CC:DD:EE:FF`:

```sh
BLE="tools/.venv/bin/python tools/host_cli.py --ble"
$BLE info
$BLE list
$BLE add "GitHub" JBSWY3DPEHPK3PXP
```

`list` and the mutating commands (`add`/`remove`/`rename`/`move`) need an authenticated
bond over BLE — pair the host once (long-press to open the pairing window,
confirm the 6-digit code on the OLED); a never-bonded host gets `NOT_AUTHORIZED`.
See [../PROTOCOL.md](../PROTOCOL.md) "Authorization".

By default a BLE command leaves the link up on exit rather than disconnecting:
the device is a trusted HID keyboard, so BlueZ holds the ACL open, and tearing it
down would make BlueZ instantly reconnect — a "disconnected"/"reconnected"
notification storm on every run. Pass `--ble-disconnect` to force a full teardown
(e.g. an unbonded one-off).

The last successful connection is saved to a shared config file (via
`platformdirs`, e.g. `~/.config/esp-otp/config.json` on Linux) that both
`host_cli.py` and `tui.py` use. Run `host_cli.py` with **no** `-p`/`--ble` and it
reconnects to that last transport automatically.

`add` accepts either a label + base32 secret or a full `otpauth://totp/...` URI
(secret and `digits`/`period`/`algorithm` are taken from the URI; explicit
`--digits/--period/--algo` override it). The secret is base32-validated before
anything is sent. Add `--dry-run` to preview what would be added — labels and
parameters, never the secret itself — without opening the port:

```sh
tools/.venv/bin/python tools/host_cli.py add "otpauth://totp/..." --dry-run
```

## tui.py — terminal console (Textual)

A full-screen terminal UI with the same capabilities as the [web console](web/):
connect over **BLE or serial**, mirror the OLED live (1 Hz), drive the button
(single / double / long), reboot, and manage secrets (list / add / remove /
rename / move) plus set-time and ping. Mouse-driven — click buttons, select table rows,
focus inputs — and keyboard works too.

```sh
tools/.venv/bin/python tools/tui.py     # BLE, finds any esp-otp by service UUID
```

Pick the transport (BLE/Serial) top-left. The target field is optional for BLE —
leave it blank to match any esp-otp by service UUID, or type a GAP name / address
(`Tasos-otp`, `AA:BB:…`) to narrow it; for serial it's a port (`/dev/ttyACM0`).
"Rename dev" changes the device's BLE name. Needs `textual` (in
`requirements.txt`); reuses the wire codec from `host_cli.py`. The OLED mirror is
drawn with block-sextant glyphs (Unicode 13 "legacy computing"); most modern
terminal fonts include them — if the panel shows boxes, switch to one that does.
Privileged
commands (screen mirror, buttons, reboot, list, add/remove/rename/move, set-time) need
an authorized/bonded link, same as everywhere else.

## otpauth_migration.py — import a Google Authenticator export

Google Authenticator's **Transfer accounts → Export** shows a QR that encodes an
`otpauth-migration://offline?data=...` URI bundling many accounts. Decode that
QR (any QR reader), then convert it to plain `otpauth://` URLs and pipe them
into `add`:

```sh
MIG='otpauth-migration://offline?data=...'          # from the export QR
tools/.venv/bin/python tools/otpauth_migration.py "$MIG" \
  | while read url; do
      tools/.venv/bin/python tools/host_cli.py -p /dev/ttyACM0 add "$url"
    done
```

Preview first without writing anything:

```sh
tools/.venv/bin/python tools/otpauth_migration.py "$MIG" \
  | while read url; do
      tools/.venv/bin/python tools/host_cli.py add "$url" --dry-run
    done
```

The converter reads URIs from arguments or stdin, prints one `otpauth://totp/`
URL per **TOTP** account to stdout (HOTP and MD5 entries are skipped; the device
does TOTP with SHA1/256/512), and needs no dependencies. `--selftest` decodes a
known vector.

## Other tools

- [clock-sync/](clock-sync/) — `ble_clock_sync.py`, a daemon that keeps the
  device clock correct over BLE. Has its own README and Docker/systemd setup.
- [web/](web/) — a browser Web Bluetooth client for the same protocol (own README).
- `gen_font.py` — regenerate `components/display/font6x8.h` from the pixel font.
- `totp-test.py` — quick TOTP cross-check against `pyotp` (needs `pip install pyotp`).
