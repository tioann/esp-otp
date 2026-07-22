# esp-otp web console

A browser page to manage the device (list / add / remove / rename / reorder secrets, set
time, ping) over the same framed protocol as [`../host_cli.py`](../host_cli.py)
and [`PROTOCOL.md`](../../PROTOCOL.md). One UI, two transports:

| Transport | Target | How |
|-----------|--------|-----|
| **Web Serial** | real device | browser opens the C3's USB‑Serial/JTAG (`/dev/ttyACM0`) directly — no server needed for the link |
| **Web Bluetooth** | real device | browser connects to the BLE management GATT service; no cable, no server |

Neither transport needs a server for the link itself — but Web Serial and Web
Bluetooth require a *secure context*, so serving the page from `localhost` is the
simplest way to satisfy that.

## Real device

```sh
tools/web/server.py           # (or any static server over https/localhost)
# open http://localhost:8000/ , pick "Device (Web Serial)", Connect,
# choose the esp-otp port in the browser prompt
```

Requirements: a Chromium browser (Chrome/Edge/Opera — Firefox/Safari have no Web
Serial), a secure context (`localhost` or `https`), and exclusive access to the
port (close any serial monitor first).

## Real device over Bluetooth

```sh
tools/web/server.py           # (or any static server over https/localhost)
# open http://localhost:8000/ , pick "Device (Web Bluetooth)", Connect,
# choose "esp-otp" in the browser's device chooser
```

Requirements: Chrome/Edge on **desktop or Android** (no iOS Safari, no Firefox),
a secure context, and Bluetooth enabled. The page filters by the `esp-otp` name
and accesses the management service by its 128‑bit UUID (`optionalServices`).
The device auto‑initiates Just‑Works pairing on connect, so mutating commands
(add/remove/rename/move) work without any extra pairing step. The HID keyboard
service is intentionally **not** used here — typing is handled by the OS, and
Web Bluetooth blocklists `0x1812` regardless.

## Display & controls

The **Display** card mirrors the OLED live (polls `GET_SCREEN` at 1 Hz while
connected) and drives the device's single physical button remotely:

- **Single** — cycle the view (home ↔ entries) / confirm a pairing prompt
- **Double** — cycle the HID keystroke target
- **Long** — open the pairing window (home) or type the current code over HID (entry)
- **Reset** — reboot the device (`REBOOT`); the link drops and reconnects

The screen mirror and the button/reset controls all need an authorized
(bonded) link, same as the other privileged commands.

## Notes

- `server.py` is stdlib‑only (no pip installs): a plain static file server.
- Management frames are identical on both transports.
- Keep the frame codec **and opcode set** in `app.js` in sync with
  `host_cli.py` if the protocol changes.
