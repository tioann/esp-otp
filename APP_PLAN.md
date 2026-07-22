# esp-otp companion app — implementation plan

Status: **planning** (no app code yet). This document captures the decisions
made so far so implementation can start from a shared understanding. The
firmware-side prerequisite (gating `SET_TIME` behind pairing) is **already done**
— see the "Firmware prerequisites" section.

## Goal

A cross-platform companion app that talks the esp-otp **management protocol**
(framed binary over USB-serial + BLE GATT, see [PROTOCOL.md](PROTOCOL.md)) to:

- manage TOTP secrets on the device (list / add / remove / rename),
- set / sync the device clock,
- run an **automatic background time-sync service** while the device is
  connected (push time on connect, then every 10 minutes).

Primary UI is graphical; a thin CLI covers the terminal. The primary
auto-sync host is the **user's phone**.

## Stack decision — Flutter (Dart)

Chosen because the two hard constraints point there:

- **BLE on all four native targets incl. Android** — `flutter_blue_plus` is the
  most mature cross-platform BLE library (Android, iOS, macOS, Windows, Linux
  from one API). This is where Rust (btleplug — Android experimental) and Go
  (weak Android) fall down.
- **GUI primary on 4 platforms incl. Android** — Flutter's core strength; single
  widget codebase.
- **TUI is only nice-to-have** — so we don't force a terminal UI into Dart;
  cover the terminal with a thin CLI (or keep the existing Python `host_cli.py`).

Runner-up if the project were Kotlin-heavy: Compose Multiplatform + Kable, but
desktop BLE (Windows/Linux) is weaker than flutter_blue_plus.

## Platform / transport matrix

| Target | BLE backend | Notes |
|--------|-------------|-------|
| Windows / Linux / macOS (app) | `flutter_blue_plus` | Linux = BlueZ, the least-mature backend; prototype early |
| Android (app) | `flutter_blue_plus` | runtime `BLUETOOTH_SCAN`/`CONNECT` perms (Android 12+) |
| iOS (app) | `flutter_blue_plus` | CoreBluetooth (UUID-based, no MAC) |
| Web — Chrome/Edge desktop | `flutter_web_bluetooth` | **different lib**; user-gesture chooser, HTTPS origin |
| Web — Chrome Android | `flutter_web_bluetooth` | works |
| Web — Safari / iOS / Firefox | — | **no Web Bluetooth**; must use the native app |

Web is a "6th target with an asterisk": full on Chrome/Edge/Chrome-Android, never
on Apple/Firefox web. There is already a proven Web Bluetooth client to port
logic from: [tools/web/](tools/web/).

## Architecture

```
lib/
  proto/            pure-Dart protocol codec — SOF framing, LEN, TYPE|SEQ,
                    CRC-16/CCITT-FALSE, little-endian ints (mirror PROTOCOL.md).
                    No UI, no BLE. Unit-tested against host_cli.py vectors.
                    Platform-agnostic → runs on web unchanged.
  transport/
    transport.dart  abstract BleTransport { connect, sendFrame, onResponse }
    ble_native.dart flutter_blue_plus      → Win/Linux/macOS/Android/iOS(app)
    ble_web.dart    flutter_web_bluetooth  → Chrome/Edge desktop + Chrome Android
    factory.dart    kIsWeb ? WebBle() : NativeBle()
  sync/
    sync_service.dart  connect → SET_TIME(now) → 10-min periodic → reconnect
                       (the same ~40-line loop as tools/ble_clock_sync.py)
  platform/
    android_fg.dart    foreground service (flutter_foreground_task) hosting sync
    ios_bg.dart        CoreBluetooth background modes + state restoration
  ui/               shared widgets — one codebase for all targets
bin/
  cli.dart          optional thin CLI reusing proto/ (the "TUI" slot)
```

The value lives in `proto/`: write and unit-test the framing/CRC once, and both
transports plus the CLI reuse it. UI is fully shared.

### GATT characteristics (from PROTOCOL.md)

- Command  (write):  `6f747001-0000-1000-8000-00805f9b34fb`
- Response (notify): `6f747002-0000-1000-8000-00805f9b34fb`

Frames keep their `SOF` on BLE too, so a single codec serves both transports.
Response reassembly across notifications relies on the `SOF`+`LEN` framing.

## Auto-sync service

Behaviour (identical to the existing Python daemon
[tools/ble_clock_sync.py](tools/ble_clock_sync.py), which already does exactly
this with a 10-minute default):

1. connect when the device is present,
2. push `SET_TIME(now)` immediately,
3. re-push every **10 minutes** while connected,
4. auto-reconnect on drop / reappear.

The loop logic is trivial and portable; the hard part is **background lifetime**,
which is per-platform:

| Host | Background story | 10-min tick |
|------|------------------|-------------|
| Android (primary) | foreground service + persistent notification | real timer ✅ |
| iOS | `bluetooth-central` bg mode + state restoration | mostly connect-driven, no guaranteed wall-clock tick ⚠️ |
| Desktop app | only while app open; optional tray + run-at-login | ✅ |
| Web | tab must stay open | not a service ❌ |
| Headless box (Pi/Docker) | existing Python daemon | ✅ (but see pairing caveat) |

Nice property: if the phone is **both** the HID keyboard host **and** the
syncer, that is **one BLE connection** doing HID notifications + management
writes — no extra link off the device's 3-link budget, and the single bond it
made for HID also authorizes `SET_TIME`.

## Security / pairing model

- Device pairing is **numeric-comparison + MITM-required, no Just Works**: the
  OLED shows a 6-digit code the host also shows; the user confirms the match with
  the device button. New bonds only accepted while the **pairing window** is open
  (long-press, ~60 s).
- `SET_TIME`, `LIST`, and all mutating ops require an **encrypted, authenticated
  bond** over BLE. Only `PING`/`GET_INFO` are open. Over USB, physical access is
  authorization.
- Consequence for the app: **pair once** (the same flow used for HID), then the
  bond authorizes management + time-sync on every reconnect. No raw secret is
  ever readable — the protocol exposes labels/metadata only; keep that invariant
  in the UI (add/remove/rename, never display secrets).

## Firmware prerequisites

- **Done (2026-07-15):** `SET_TIME` is now gated behind the authenticated bond
  (`requires_auth()` in [components/protocol/dispatch.c](components/protocol/dispatch.c)).
  Flashed and verified on device. PROTOCOL.md and the clock-sync docs are
  updated. This is why the auto-syncer must bond once rather than run unbonded.
- No further firmware work is required for the app; the management + HID GATT
  services already exist.

## Web-specific gotchas

- **HTTPS required** (or `localhost`) — Web Bluetooth only on a secure origin.
- **User gesture** needed to open the device chooser; no silent auto-scan.
- Silent reconnect is limited (`getDevices()` on newer Chrome helps, not
  guaranteed). No background — tab must stay open.
- Feature-detect `navigator.bluetooth`; if absent, show a "use the app" fallback.

## Milestones (proposed)

1. **`proto/` codec + tests** — framing, CRC-16/CCITT-FALSE, request/response
   types; unit tests against known vectors from `host_cli.py`. No BLE yet.
2. **Native transport + minimal UI** — `flutter_blue_plus`, connect + `GET_INFO`
   + `LIST` on desktop/Android; render the secret list.
3. **Pairing flow** — trigger OS pairing, guide the user through the OLED
   numeric-comparison confirm; handle `NOT_AUTHORIZED`.
4. **Secret management** — add (base32 + `otpauth://`), remove, rename; QR import
   (`mobile_scanner`) mirroring `otpauth_migration.py`.
5. **Sync service** — `sync_service.dart`; Android foreground service; iOS
   best-effort background; desktop "while open".
6. **Web target** — `flutter_web_bluetooth` transport behind the same interface;
   port logic from `tools/web/`.
7. **Polish** — remembered device, reconnect UX, error surfaces, packaging per
   platform.

## Open questions / decisions deferred

- Do we keep the Python daemon for headless syncing, or make the phone the sole
  syncer? (Gating `SET_TIME` makes headless awkward — needs a one-time
  interactive pair.)
- iOS background sync fidelity — how important is a guaranteed 10-min tick vs
  connect-driven sync?
- Package the CLI (`bin/cli.dart`) or lean on the existing Python `host_cli.py`
  for the terminal use case?
- Linux desktop BLE reliability (BlueZ backend) — validate before committing to
  it as a first-class target.
