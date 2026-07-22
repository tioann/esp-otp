# esp-otp BLE clock-sync

Keeps **every** esp-otp device's clock correct over BLE. Whenever a device is in
range it connects, pushes host time immediately, then re-pushes every 10 minutes
while connected. Several devices are synced at once (one connection each), and it
reconnects on its own when a device drops or reappears.

Devices are identified purely by their advertised **management service UUID**, so
the daemon needs no per-device name/config and a `SET_NAME` rename never breaks
it — any esp-otp in range is synced. Pin `--address` to restrict it to one.

`SET_TIME` requires an encrypted, authenticated bond (see
[../../PROTOCOL.md](../../PROTOCOL.md)). **Pair the host once** — long-press the
device button to open the pairing window, then confirm the 6-digit code shown on
the OLED. After that the daemon reconnects on the stored bond and syncs with no
further prompts; a never-bonded host is rejected with `NOT_AUTHORIZED`.

> Note: because the one-time pairing needs a button press + OLED confirm, this
> daemon suits a host that can be paired interactively once (e.g. a laptop or the
> user's phone), not a truly headless box that can never touch the device.

The daemon is [../ble_clock_sync.py](../ble_clock_sync.py); it reuses the wire
codec from [../host_cli.py](../host_cli.py) and swaps serial for BLE (`bleak`).

## Config

| env / flag                    | default   | meaning                          |
|-------------------------------|-----------|----------------------------------|
| `ESP_OTP_ADDRESS` / `--address` | –       | restrict to one BLE address (default: all esp-otp) |
| `ESP_OTP_INTERVAL` / `--interval` | `600` | refresh seconds                  |
| `ESP_OTP_SCAN` / `--scan`     | `10`      | per-scan timeout seconds         |

## Run directly

```sh
pip install bleak
tools/ble_clock_sync.py                 # sync every esp-otp in range, forever
tools/ble_clock_sync.py --address AA:BB:CC:DD:EE:FF   # just one device
```

## Docker

bleak reaches BLE through the **host's** bluetoothd over D-Bus, so the container
needs the system D-Bus socket and host networking — both are set in the compose
file.

```sh
docker compose -f tools/clock-sync/docker-compose.yml up -d --build
docker logs -f esp-otp-clock-sync
```

Or plain docker:

```sh
docker build -f tools/clock-sync/Dockerfile -t esp-otp-clock-sync tools
docker run -d --name esp-otp-clock-sync --restart unless-stopped \
  --network host --cap-add NET_ADMIN \
  -v /run/dbus/system_bus_socket:/run/dbus/system_bus_socket \
  -e ESP_OTP_INTERVAL=600 \
  esp-otp-clock-sync
```

## systemd (no Docker)

See the install steps in
[esp-otp-clock-sync.service](esp-otp-clock-sync.service).

## Notes

- The device advertises the management **service UUID** (plus an HID-keyboard
  appearance), so the daemon matches on that service — rename-proof and unique to
  esp-otp — rather than on the changeable GAP name.
- If BlueZ has bonded/trusted the device it may auto-connect and flap the link
  (see project notes). Pin `--address` and let one owner manage the link if you
  see churn.
