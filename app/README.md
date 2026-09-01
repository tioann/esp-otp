# esp-otp companion app (Android)

A Flutter Android app that speaks the esp-otp **management protocol**
([PROTOCOL.md](../PROTOCOL.md)) over BLE. It gives the host CLI's functionality a
GUI and adds a background service that time-syncs any esp-otp the moment it
connects — the phone port of [tools/ble_clock_sync.py](../tools/ble_clock_sync.py).

Everything builds **inside a container** — no local Flutter/Android SDK needed.

## Build

From the repo root:

```bash
docker compose run --rm app-build            # debug APK (sideload-ready)
docker compose run --rm app-build release    # release APK (unsigned)
```

Output: `app/build/app/outputs/flutter-apk/app-debug.apk`.

The container scaffolds the `android/` platform folder on first run (it is
git-ignored, never hand-edited) and layers our permission + foreground-service
manifest from [android_overlay/](android_overlay/) on top. The protocol unit
tests run as part of the build.

Install on a device:

```bash
adb install -r app/build/app/outputs/flutter-apk/app-debug.apk
```

## What it does

**Tokens tab** — list / add / remove / rename / reorder TOTP secrets. Add from a
base32 secret + label or by pasting an `otpauth://totp/…` URI (parsed the same
way as `host_cli.py`). Secrets are never displayed — the protocol only exposes
labels/metadata.

**Device tab** — firmware/RTC/time-valid/capacity info, a live mirror of the
OLED (GET_SCREEN), *Sync time now*, *Pair*, *Set name*, *Reboot*, and button
injection (single/double/long).

**Background clock sync** — a toggle on the home screen starts an Android
foreground service. It scans for esp-otp devices, connects each, pushes
`SET_TIME` immediately, then refreshes every 10 minutes and reconnects on drop —
identical behaviour to `ble_clock_sync.py`.

## Pairing (required for most commands)

`LIST`, `SET_TIME` and all mutating ops need an **authenticated bond**. Pair
once:

1. Long-press the device button to open its ~60 s pairing window.
2. Tap **Pair** (Device tab). Android shows a passkey prompt.
3. Enter the 6-digit code shown on the device OLED and confirm on the device.

After that the bond authorises management + background sync on every reconnect.
Only `PING`/`GET_INFO` work unpaired.

## Architecture

```
lib/
  proto/      pure-Dart protocol codec (framing, CRC-16/CCITT-FALSE, otpauth,
              models) — the twin of host_cli.py; unit-tested in test/.
  ble/        flutter_blue_plus transport + OtpDevice client (all opcodes).
  sync/       SyncEngine (ble_clock_sync.py port) + foreground-service handler.
  ui/         scan/connect, device tabs, add-token, live OLED view.
```

The codec has no BLE/UI dependency, so the same logic serves the UI, the
background isolate, and (later) a web/serial transport.

## Status / limitations

- **Android only** for now (per APP_PLAN.md milestone 2/5). iOS/desktop/web use
  the same `proto/` + a different transport later.
- Written against Flutter 3.24 / `flutter_blue_plus` 1.32 / `flutter_foreground_task`
  8.10. Not yet validated on hardware — the codec is unit-tested; the BLE and
  background-service paths need an on-device pass.
- Some Android OEMs throttle background BLE; the toggle requests battery-
  optimisation exemption, but aggressive power managers may still pause the
  service.
