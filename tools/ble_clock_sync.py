#!/usr/bin/env python3
"""esp-otp BLE clock-sync daemon.

Keeps **every** esp-otp device's clock in sync over BLE. It scans continuously
and, for each device present, holds an open connection. The sync cadence is:

  1. on connect, a device is time-synced (SET_TIME) immediately,
  2. a single shared ticker then pushes host time to *all* connected devices
     every REFRESH_INTERVAL seconds — one timer for the whole fleet, not a
     separate wait per device,
  3. a dropped device reconnects automatically (and re-syncs at once) when it
     comes back.

Several devices are synced concurrently — one connection per device. Devices are
identified purely by their advertised **management service UUID** (not their GAP
name, which SET_NAME can change), so the daemon needs no per-device config: any
esp-otp in range gets synced. Pass --address to restrict it to a single device.

SET_TIME requires an encrypted, authenticated bond (see PROTOCOL.md
"Authorization"). Pair the host with each device **once** — open the pairing
window with a long-press and confirm the 6-digit code on the OLED — after which
this daemon reconnects on the stored bond and syncs without further prompts. A
never-bonded host is rejected with NOT_AUTHORIZED.

The wire codec (framing, CRC, SET_TIME opcode) is reused from host_cli.py; this
module only swaps the serial transport for a BLE GATT one via `bleak`.

Config is via CLI flags or environment variables:

    ESP_OTP_ADDRESS     restrict to one exact BLE address (default: all esp-otp)
    ESP_OTP_INTERVAL    refresh seconds             (default: 600)
    ESP_OTP_SCAN        per-scan timeout seconds     (default: 10)

Examples:
    tools/ble_clock_sync.py                       # sync every esp-otp in range
    tools/ble_clock_sync.py --address AA:BB:CC:DD:EE:FF   # just one device
    ESP_OTP_INTERVAL=300 tools/ble_clock_sync.py  # refresh every 5 min
"""
import argparse
import asyncio
import logging
import os
import signal
import struct
import time

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

from host_cli import OP_SET_TIME, RESP_BIT, STATUS, Parser, encode

# Management service + characteristics (see PROTOCOL.md "BLE GATT layout").
SVC_UUID = "6f747000-0000-1000-8000-00805f9b34fb"   # Management service
CMD_UUID = "6f747001-0000-1000-8000-00805f9b34fb"   # Command  (write)
RSP_UUID = "6f747002-0000-1000-8000-00805f9b34fb"   # Response (notify)

log = logging.getLogger("clock-sync")


class ClockSyncError(Exception):
    """A SET_TIME exchange failed (bad status, no response, or GATT error)."""


async def push_time(client: BleakClient, seq: int, label: str = "",
                    timeout: float = 5.0) -> int:
    """Send one SET_TIME(now) and wait for its OK response. Returns next seq."""
    now = int(time.time())
    frame = encode(OP_SET_TIME, seq, struct.pack("<I", now))

    parser = Parser()
    done = asyncio.get_running_loop().create_future()

    def on_notify(_char, data: bytearray):
        for b in data:
            got = parser.push(b)
            if got and got[0] == (OP_SET_TIME | RESP_BIT) and got[1] == seq:
                if not done.done():
                    done.set_result(got[2])

    await client.start_notify(RSP_UUID, on_notify)
    try:
        # Write-with-response so we know the device accepted the frame.
        await client.write_gatt_char(CMD_UUID, frame, response=True)
        try:
            payload = await asyncio.wait_for(done, timeout)
        except asyncio.TimeoutError:
            raise ClockSyncError("no SET_TIME response") from None
    finally:
        try:
            await client.stop_notify(RSP_UUID)
        except BleakError:
            pass

    status = payload[0] if payload else 0xFF
    if status != 0:
        raise ClockSyncError(f"device error: {STATUS.get(status, status)}")
    prefix = f"{label} " if label else ""
    log.info("%sclock set to %d (%s UTC)", prefix, now,
             time.strftime("%H:%M:%S", time.gmtime(now)))
    return (seq + 1) & 0xFF


async def discover_devices(args):
    """Return every esp-otp currently advertising (matched by the management
    service UUID, so rename-proof). With --address, restrict to that one device."""
    if args.address:
        dev = await BleakScanner.find_device_by_address(args.address, timeout=args.scan)
        return [dev] if dev else []
    found = await BleakScanner.discover(timeout=args.scan, return_adv=True)
    return [d for d, adv in found.values()
            if SVC_UUID in (u.lower() for u in adv.service_uuids)]


class Conn:
    """A live device connection tracked by the shared refresh ticker."""

    def __init__(self, client, label):
        self.client = client
        self.label = label
        self.seq = 1


async def serve_connection(dev, stop: asyncio.Event, conns: dict):
    """Hold one device's connection open: sync its clock immediately on connect,
    register it in `conns` so the shared ticker refreshes it, then stay until the
    link drops or shutdown. Self-contained — logs and returns on any error so the
    manager can reap it and rediscover the device on the next scan."""
    label = f"{dev.name or '?'} ({dev.address})"
    disconnected = asyncio.Event()

    def on_disconnect(_client):
        log.warning("%s disconnected", label)
        disconnected.set()

    try:
        async with BleakClient(dev, disconnected_callback=on_disconnect) as client:
            log.info("connected to %s", label)
            conn = Conn(client, label)
            conn.seq = await push_time(client, conn.seq, label)  # immediate sync
            conns[dev.address] = conn
            try:
                # Just hold the link; the shared ticker does the periodic refresh.
                waiters = [asyncio.create_task(disconnected.wait()),
                           asyncio.create_task(stop.wait())]
                _, pending = await asyncio.wait(
                    waiters, return_when=asyncio.FIRST_COMPLETED)
                for t in pending:
                    t.cancel()
            finally:
                conns.pop(dev.address, None)
    except (BleakError, ClockSyncError) as e:
        log.warning("%s: sync ended (%s)", label, e)


async def refresh_ticker(interval: float, stop: asyncio.Event, conns: dict):
    """Single shared loop: every `interval` s, push host time to every currently
    connected device. Devices are synced once immediately on connect (in
    serve_connection); this keeps them corrected thereafter with one timer for the
    whole fleet rather than a separate wait per device."""
    while not stop.is_set():
        try:
            await asyncio.wait_for(stop.wait(), timeout=interval)
            return  # shutdown
        except asyncio.TimeoutError:
            pass
        for conn in list(conns.values()):
            try:
                conn.seq = await push_time(conn.client, conn.seq, conn.label)
            except (BleakError, ClockSyncError) as e:
                log.warning("%s: refresh failed (%s)", conn.label, e)


async def run(args, stop: asyncio.Event):
    """Supervisor: one shared refresh ticker plus one serve_connection task per
    device — spawn a task when a new esp-otp appears, reap it when it ends."""
    conns = {}   # address -> Conn  (currently connected, refreshed by the ticker)
    tasks = {}   # address -> serve_connection Task
    ticker = asyncio.create_task(refresh_ticker(args.interval, stop, conns))
    try:
        while not stop.is_set():
            for addr in [a for a, t in tasks.items() if t.done()]:
                tasks.pop(addr)
            try:
                devs = await discover_devices(args)
            except BleakError as e:
                log.warning("scan failed: %s", e)
                devs = []
            for dev in devs:
                if dev.address not in tasks:
                    log.info("found %s (%s); starting sync", dev.name or "?", dev.address)
                    tasks[dev.address] = asyncio.create_task(
                        serve_connection(dev, stop, conns))
            # Pause a scan cycle before rescanning; wake immediately on shutdown.
            try:
                await asyncio.wait_for(stop.wait(), timeout=args.scan)
            except asyncio.TimeoutError:
                pass
    finally:
        ticker.cancel()
        for t in tasks.values():
            t.cancel()
        await asyncio.gather(ticker, *tasks.values(), return_exceptions=True)


def parse_args():
    ap = argparse.ArgumentParser(description="esp-otp BLE clock-sync daemon")
    ap.add_argument("--address", default=os.getenv("ESP_OTP_ADDRESS") or None,
                    help="restrict to one exact BLE address (default: sync all esp-otp)")
    ap.add_argument("--interval", type=float,
                    default=float(os.getenv("ESP_OTP_INTERVAL", "600")),
                    help="refresh interval in seconds (default: 600)")
    ap.add_argument("--scan", type=float,
                    default=float(os.getenv("ESP_OTP_SCAN", "10")),
                    help="per-scan timeout in seconds (default: 10)")
    return ap.parse_args()


def main():
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S")
    args = parse_args()

    async def amain():
        stop = asyncio.Event()
        loop = asyncio.get_running_loop()
        for sig in (signal.SIGINT, signal.SIGTERM):
            loop.add_signal_handler(sig, stop.set)
        await run(args, stop)
        log.info("shutting down")

    asyncio.run(amain())


if __name__ == "__main__":
    main()
