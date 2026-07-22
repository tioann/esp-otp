# esp-otp wire protocol

One framed, binary request/response protocol shared by **both** transports:

- **USB-serial** (the C3's native USB-Serial/JTAG, also reachable via Web Serial)
- **BLE** management GATT service

The device is the server; the app/web page is the client. Every request gets
exactly one response. All multi-byte integers are **little-endian**.

> Note: this protocol carries *management* traffic (configure secrets, set
> time). Typing a code as keystrokes is a separate **BLE HID** function, not a
> command in this protocol. The C3 cannot type over USB (see README).

## Framing

```
+------+-----------+--------+--------+-------------------+----------+
| SOF  |  LEN (2)   | TYPE 1 | SEQ 1  | PAYLOAD (LEN-2)   | CRC16 2  |
| 0x7E | uint16 LE  | uint8  | uint8  | ...               | uint16LE |
+------+-----------+--------+--------+-------------------+----------+
```

- `SOF` = `0x7E`, lets a serial reader resync to a frame boundary.
- `LEN` = byte count of `TYPE + SEQ + PAYLOAD`.
- `SEQ` = client-chosen tag; the device echoes it so replies can be matched.
- `CRC16` = CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`) over
  `TYPE + SEQ + PAYLOAD`.

On BLE the identical frame bytes are written to the **Command** characteristic
and the response is delivered as a notification on the **Response**
characteristic. `SOF` is retained on BLE too so a single codec serves both
transports.

### Responses

A response reuses the request's `TYPE` with the high bit set (`TYPE | 0x80`)
and the same `SEQ`. The payload always begins with a 1-byte **status**:

| status | name           | meaning                              |
|-------:|----------------|--------------------------------------|
| `0x00` | OK             | success                              |
| `0x01` | BAD_REQUEST    | malformed frame / wrong length       |
| `0x02` | FULL           | secret store is full                 |
| `0x03` | NOT_FOUND      | no record with that id               |
| `0x04` | INVALID_ARG    | bad field (e.g. invalid base32)      |
| `0x05` | NOT_AUTHORIZED | link not bonded/encrypted (BLE)      |
| `0x06` | CRC_ERROR      | CRC mismatch                         |
| `0x07` | UNSUPPORTED    | unknown opcode                       |

## Authorization

- **BLE**: commands are authorized per-opcode over an **encrypted, bonded**
  link. The mutating ops (`ADD`/`REMOVE`/`RENAME`/`MOVE`/`SET_NAME`), `LIST` (its labels reveal
  which accounts are stored), `SET_TIME` (only a paired host may steer the
  clock TOTP depends on), `GET_SCREEN` (the panel can show a live code or the
  pairing passkey), `INJECT_BUTTON` (it can open pairing / type codes) **and**
  `REBOOT` require bonding; an unbonded attempt is rejected with
  `NOT_AUTHORIZED`. Only `PING` and `GET_INFO` stay open.
- Bonding uses **passkey-display, MITM-required** pairing: the device shows a
  6-digit code on its OLED that the host must enter (no Just Works fallback).
  "Authorized" means an **authenticated** bonded link, so a peer that skips the
  passkey is never authorized.
- New bonds are only accepted while a **pairing window** is open. Long-press the
  device button to open it (~60s, or until one host pairs); outside the window
  new pairing attempts are refused and re-pairing is ignored, so a configured
  device can't be hijacked or have its bond overwritten. Already-bonded hosts
  reconnect any time.
- **USB**: physical possession is the authorization; all commands are accepted
  (including `SET_TIME` — a wired host is already trusted).
- A future device-PIN may add a second factor over mutating commands; reserved
  but not yet implemented.

## Commands

### `0x01 PING`
Request payload: none. Response payload: `status` only. Liveness check.

### `0x02 GET_INFO`
Request payload: none.
Response payload:

| field      | type   | notes                                   |
|------------|--------|-----------------------------------------|
| status     | u8     |                                         |
| fw_major   | u8     |                                         |
| fw_minor   | u8     |                                         |
| fw_patch   | u8     |                                         |
| flags      | u8     | bit0 = RTC present, bit1 = time valid   |
| count      | u16    | stored secrets                          |
| capacity   | u16    | maximum secrets                         |
| name_len   | u8     | length of the BLE device name           |
| name       | u8[]   | UTF-8 device name, not NUL-terminated   |

The `name_len`/`name` fields were added after v1's initial fields; they are
appended, so a client that reads only the first 9 bytes stays compatible.

### `0x03 SET_TIME`
Request payload: `unix` (u32) — seconds since epoch, UTC.
Response payload: `status`.
Sets the system clock; if a DS3231 is present it is written too.

### `0x04 LIST`
Request payload: `start` (u8) — index to begin at (for pagination on small BLE
MTUs). Response payload:

| field    | type | notes                                |
|----------|------|--------------------------------------|
| status   | u8   |                                      |
| total    | u16  | total secret count                   |
| n        | u8   | number of entries in THIS response   |
| entries  | …    | `n` repetitions of the record below  |

Each entry (**never includes the secret**):

| field     | type | notes                |
|-----------|------|----------------------|
| id        | u16  | stable identifier    |
| digits    | u8   |                      |
| period    | u16  | seconds              |
| algo      | u8   | 0=SHA1,1=SHA256,2=SHA512 |
| label_len | u8   |                      |
| label     | u8[] | UTF-8, not NUL-term  |

The device packs as many entries as fit the negotiated MTU; the client repeats
`LIST` with an advanced `start` until it has `total` entries.

### `0x05 ADD`
Request payload:

| field       | type | notes                          |
|-------------|------|--------------------------------|
| digits      | u8   | 0 ⇒ default 6                  |
| period      | u16  | 0 ⇒ default 30                 |
| algo        | u8   | 0/1/2                          |
| label_len   | u8   | ≤ 31                           |
| label       | u8[] | UTF-8                          |
| secret_len  | u8   | length of the base32 string    |
| secret_b32  | u8[] | base32 secret (RFC 4648)       |

The device base32-decodes and stores the raw key. Response payload:
`status`, then `id` (u16) on success.

### `0x06 REMOVE`
Request payload: `id` (u16). Response payload: `status`.

### `0x07 RENAME`
Request payload: `id` (u16), `label_len` (u8), `label` (u8[]).
Response payload: `status`.

### `0x0B MOVE`
Request payload: `id` (u16), `new_index` (u8). Response payload: `status`.

Repositions the token to cycle index `new_index` (0-based, `0`..`count-1`),
shifting the records in between to close the gap — the same order LIST returns
and the on-device view cycles through. The stable `id` is unchanged; only the
position moves. `NOT_FOUND` if no record has that id, `INVALID_ARG` if
`new_index >= count`. Moving a record to its current index is a no-op `OK` (no
flash write). Requires an authorized link.

### `0x08 GET_SCREEN`
Request payload: `start` (u16) — byte offset into the framebuffer (for
pagination on small BLE MTUs). Response payload:

| field  | type | notes                                            |
|--------|------|--------------------------------------------------|
| status | u8   |                                                  |
| total  | u16  | full framebuffer size in bytes (`width`×`pages`) |
| width  | u8   | panel width in columns (72)                      |
| pages  | u8   | byte-rows; each byte is 8 vertical pixels (5)    |
| data   | u8[] | framebuffer bytes `[start, start+len(data))`     |

Returns a monochrome snapshot of exactly what the OLED shows. The buffer is
column-major by page: byte `page*width + col` holds an 8-pixel vertical run,
bit0 = top row. The device packs as many bytes as fit the negotiated MTU; the
client repeats with an advanced `start` until it has `total` bytes (an empty
`data` also signals the end). Requires an authorized link — the panel may be
displaying a live TOTP code or the pairing passkey.

### `0x09 INJECT_BUTTON`
Request payload: `gesture` (u8) — `1` single, `2` double, `3` long. Response
payload: `status`.

Posts a synthetic button event onto the same queue the physical button feeds, so
it drives the on-device UI identically to a real press: **single** cycles the
view (or confirms a pairing numeric-comparison), **double** cycles the HID
keystroke target, **long** opens the pairing window (home screen) or types the
current code over BLE HID (on an entry). Requires an authorized link — it can
open pairing and trigger HID typing. `INVALID_ARG` for a gesture outside 1–3.

### `0x0A REBOOT`
Request payload: none. Response payload: `status`.

Restarts the device. The `OK` response is sent first; the actual restart is
deferred ~200 ms so the reply reaches the host before the CPU resets. The BLE
link drops on reboot and (for a bonded/trusted host) reconnects a few seconds
later. Requires an authorized link.

### `0x0C SET_NAME`
Request payload: `name_len` (u8), `name` (u8[]) — UTF-8, not NUL-terminated.
Response payload: `status`.

Changes the BLE advertised device name (the GAP name shown while scanning),
persisted in NVS so it survives reboots. Handy when several people share one
device and the default `esp-otp` name is ambiguous. `name_len` must be
`1..20` (`INVALID_ARG` otherwise; `BAD_REQUEST` if it overruns the frame). The
device applies the new name immediately and re-emits its advertising, so a fresh
scan shows it at once; already-connected links keep their session. Requires an
authorized link.

## BLE GATT layout

Management service and characteristics (128-bit, base
`6f7470xx-0000-1000-8000-00805f9b34fb`, "otp" in the first bytes):

| UUID                                   | characteristic | properties              | role                          |
|----------------------------------------|----------------|-------------------------|-------------------------------|
| `6f747000-0000-1000-8000-00805f9b34fb` | (service)      | —                       | management service            |
| `6f747001-0000-1000-8000-00805f9b34fb` | Command        | Write, Write No Response | client → device request frame |
| `6f747002-0000-1000-8000-00805f9b34fb` | Response       | Notify                  | device → client response frame |

The device **advertises** this 128-bit management service UUID (alongside the
HID service and *Keyboard* appearance), so a scanner can identify an esp-otp by
service type — stable even after the GAP device name is changed with `SET_NAME`.
The device name is carried in the scan response. Match on the service UUID (not
the name) to find a device reliably; the clock-sync daemon does this.

The Command characteristic itself carries no encryption flag, so a fresh host
can still reach `PING`/`GET_INFO`; per-command authorization is enforced in
dispatch — the mutating ops (incl. `MOVE`/`SET_NAME`), `LIST`, `SET_TIME`, `GET_SCREEN`, `INJECT_BUTTON`
**and** `REBOOT` require an encrypted/bonded link, returning `NOT_AUTHORIZED`
otherwise. The device bounds each `LIST` response to the negotiated ATT MTU.

The device also exposes a standard **HID-over-GATT** keyboard (service `0x1812`,
appearance *Keyboard*, report-protocol, 8-byte input report) plus a Device
Information service (`0x180A`, PnP ID) for the double-click "type the code"
feature. Pairing is passkey-display (6-digit code shown on the OLED); the HID
input report requires an encrypted link. That keyboard is independent of this
management protocol.

## Versioning

`GET_INFO` reports firmware version. This document describes **protocol v1**.
Backwards-incompatible changes bump the firmware major and are negotiated by
the client reading `GET_INFO` first.
