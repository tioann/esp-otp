# esp-otp BLE clock-sync daemon.
#
# Build context is tools/ (so both ble_clock_sync.py and its host_cli.py codec
# are in reach):
#
#   docker build -f tools/clock-sync/Dockerfile -t esp-otp-clock-sync tools
#
# BLE is reached through the HOST's bluetoothd over D-Bus, so the container
# needs the system D-Bus socket and host networking (see docker-compose.yml).
FROM python:3.12-slim

WORKDIR /app

COPY clock-sync/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY host_cli.py ble_clock_sync.py ./

ENTRYPOINT ["python", "ble_clock_sync.py"]
