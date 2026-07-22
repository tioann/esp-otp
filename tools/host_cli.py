#--!/usr/bin/env python3
"""esp-otp host CLI / reference client for the wire protocol (see PROTOCOL.md).

Talks the framed protocol over either transport:
  * a serial port (the C3's USB-Serial/JTAG, e.g. /dev/ttyACM0), via -p/--port;
  * BLE (the management GATT service), via --ble (needs `bleak`).
Doubles as the executable reference for the Android/web teams.

Examples:
    tools/host_cli.py -p /dev/ttyACM0 info
    tools/host_cli.py -p /dev/ttyACM0 settime           # push host time now
    tools/host_cli.py -p /dev/ttyACM0 add "GitHub" JBSWY3DPEHPK3PXP
    tools/host_cli.py -p /dev/ttyACM0 add "otpauth://totp/GitHub:me?secret=JBSWY3DPEHPK3PXP&issuer=GitHub"
    tools/host_cli.py -p /dev/ttyACM0 list

    tools/host_cli.py --ble info                        # find any esp-otp by service UUID
    tools/host_cli.py --ble --name "Tasos-otp" list     # narrow by GAP name
    tools/host_cli.py --ble --address AA:BB:CC:DD:EE:FF list

BLE note: mutating commands (add/remove/rename/move) and list require an authenticated
bond — pair the host with the device once (long-press to open the pairing window,
confirm the 6-digit code on the OLED). See PROTOCOL.md "Authorization".

Google Authenticator (otpauth-migration://) exports: convert to otpauth:// URLs
first with tools/otpauth_migration.py, then feed those to `add`.
    tools/host_cli.py -p /dev/ttyACM0 remove 1
    tools/host_cli.py --selftest                        # framing tests, no device
"""
import argparse
import json
import os
import struct
import sys
import time

SOF = 0x7E
MAX_PAYLOAD = 250

# Management service + characteristics (see PROTOCOL.md "BLE GATT layout"). The
# device advertises BLE_SVC_UUID, so we identify it by service type (stable
# across a SET_NAME rename) rather than by its changeable GAP name.
BLE_SVC_UUID = "6f747000-0000-1000-8000-00805f9b34fb"   # Management service
BLE_CMD_UUID = "6f747001-0000-1000-8000-00805f9b34fb"   # Command  (write)
BLE_RSP_UUID = "6f747002-0000-1000-8000-00805f9b34fb"   # Response (notify)

# Opcodes
(OP_PING, OP_GET_INFO, OP_SET_TIME, OP_LIST, OP_ADD, OP_REMOVE, OP_RENAME,
 OP_GET_SCREEN, OP_INJECT_BUTTON, OP_REBOOT, OP_MOVE, OP_SET_NAME) = range(1, 13)
RESP_BIT = 0x80

# INJECT_BUTTON gesture codes (mirror the firmware button_event_t).
BUTTON = {"single": 1, "double": 2, "long": 3}

STATUS = {
    0: "OK", 1: "BAD_REQUEST", 2: "FULL", 3: "NOT_FOUND",
    4: "INVALID_ARG", 5: "NOT_AUTHORIZED", 6: "CRC_ERROR", 7: "UNSUPPORTED",
}
ALGO = {0: "SHA1", 1: "SHA256", 2: "SHA512"}


# --------------------------------------------------------------- shared config
# host_cli and tui.py persist the last successful connection here so either tool
# reconnects without re-specifying the transport. platformdirs picks the correct
# per-OS location (~/.config/esp-otp on Linux, Application Support on macOS,
# %LOCALAPPDATA% on Windows) instead of hard-coding a path.

def config_path() -> str:
    try:
        from platformdirs import user_config_dir
        base = user_config_dir("esp-otp")
    except ImportError:  # fall back to a sensible Linux-ish default
        base = os.path.join(os.path.expanduser("~"), ".config", "esp-otp")
    return os.path.join(base, "config.json")


def load_last_conn():
    """Return the saved {'kind','target'} for the last good connection, or None.
    A blank BLE target is valid (match any esp-otp by service UUID); serial
    needs a port."""
    try:
        with open(config_path()) as f:
            d = json.load(f)
        kind, target = d.get("kind"), d.get("target", "")
        if kind == "ble":
            return {"kind": "ble", "target": target}
        if kind == "serial" and target:
            return {"kind": "serial", "target": target}
    except (OSError, ValueError):
        pass
    return None


def save_last_conn(kind, target):
    """Persist the last good connection; best-effort (never raises)."""
    try:
        path = config_path()
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            json.dump({"kind": kind, "target": target}, f)
    except OSError:
        pass


def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF)."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def encode(type_: int, seq: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too large")
    body = bytes([type_, seq]) + payload
    frame = bytes([SOF]) + struct.pack("<H", len(body)) + body
    return frame + struct.pack("<H", crc16(body))


class Parser:
    """Streaming frame extractor, mirrors the C parser."""

    def __init__(self):
        self.reset()

    def reset(self):
        self.state = "SOF"
        self.body_len = 0
        self.buf = bytearray()

    def push(self, byte: int):
        """Feed one byte; return (type, seq, payload) on a valid frame."""
        if self.state == "SOF":
            if byte == SOF:
                self.state = "LEN0"
        elif self.state == "LEN0":
            self.body_len = byte
            self.state = "LEN1"
        elif self.state == "LEN1":
            self.body_len |= byte << 8
            if not (2 <= self.body_len <= 2 + MAX_PAYLOAD):
                self.reset()
            else:
                self.buf = bytearray()
                self.state = "BODY"
        elif self.state == "BODY":
            self.buf.append(byte)
            if len(self.buf) == self.body_len + 2:
                body = bytes(self.buf[:self.body_len])
                crc_recv = self.buf[self.body_len] | (self.buf[self.body_len + 1] << 8)
                self.reset()
                if crc16(body) == crc_recv:
                    return body[0], body[1], body[2:]
        return None


class Device:
    def __init__(self, port, baud=115200, timeout=2.0):
        import serial  # lazy import so --selftest needs no pyserial
        self.ser = serial.Serial(port, baud, timeout=0.1)
        self.timeout = timeout
        self.parser = Parser()
        self.seq = 0

    def request(self, type_, payload=b""):
        self.seq = (self.seq + 1) & 0xFF
        seq = self.seq
        self.ser.write(encode(type_, seq, payload))
        deadline = time.time() + self.timeout
        while time.time() < deadline:
            chunk = self.ser.read(64)
            for b in chunk:
                frame = self.parser.push(b)
                if frame and frame[0] == (type_ | RESP_BIT) and frame[1] == seq:
                    return frame[2]
        raise TimeoutError(f"no response to opcode 0x{type_:02x}")


class AmbiguousDeviceError(Exception):
    """More than one esp-otp matched the scan; the caller must narrow it."""


async def scan_esp_otp(name=None, scan_timeout=10.0):
    """Scan the whole window and return every advertised esp-otp (matched by the
    management service UUID, so rename-proof) whose GAP name equals `name` when
    given (case-insensitive). Returns a list of BLEDevice — 0, 1, or many."""
    from bleak import BleakScanner  # lazy import; --ble only
    want = name.lower() if name else None
    found = await BleakScanner.discover(timeout=scan_timeout, return_adv=True)
    out = []
    for dev, adv in found.values():
        if BLE_SVC_UUID not in (u.lower() for u in adv.service_uuids):
            continue
        if want is not None and (dev.name or "").lower() != want:
            continue
        out.append(dev)
    return out


def choose_one_esp_otp(devs, name=None):
    """Pick the single matching device or fail loudly. Raises TimeoutError when
    none matched and AmbiguousDeviceError (listing them) when more than one did."""
    if not devs:
        raise TimeoutError(f"esp-otp device not found ({name or 'by service'!r})")
    if len(devs) > 1:
        listing = "\n".join(f"  {d.name or '?'}  {d.address}" for d in devs)
        raise AmbiguousDeviceError(
            f"{len(devs)} esp-otp devices match — narrow with --name or "
            f"--address:\n{listing}")
    return devs[0]


class BleDevice:
    """BLE (GATT) transport twin of Device: same .request() surface, over bleak.

    Holds one connection open for the whole command. Each request writes a frame
    to the Command characteristic and waits for the matching Response
    notification, mirroring Device.request() so the cmd_* handlers don't care
    which transport they got.
    """

    def __init__(self, name=None, address=None, scan_timeout=10.0, timeout=5.0):
        import asyncio  # lazy so the serial path / --selftest need no asyncio work
        self.timeout = timeout
        self.parser = Parser()
        self.seq = 0
        self._pending = None            # (seq, resp_type, future) for the in-flight request
        self.loop = asyncio.new_event_loop()
        self.loop.run_until_complete(self._connect(name, address, scan_timeout))

    async def _connect(self, name, address, scan_timeout):
        from bleak import BleakClient, BleakScanner  # lazy import; --ble only
        if address:
            dev = await BleakScanner.find_device_by_address(address, timeout=scan_timeout)
            if dev is None:
                raise TimeoutError(f"esp-otp device not found ({address!r})")
        else:
            # Match by advertised management service UUID (rename-proof); an
            # optional name narrows it. Fail loudly if several still match.
            dev = choose_one_esp_otp(await scan_esp_otp(name, scan_timeout), name)
        self.client = BleakClient(dev)
        await self.client.connect()
        await self.client.start_notify(BLE_RSP_UUID, self._on_notify)

    def _on_notify(self, _char, data):
        for b in data:
            frame = self.parser.push(b)
            if not (frame and self._pending):
                continue
            seq, resp_type, fut = self._pending
            if frame[0] == resp_type and frame[1] == seq and not fut.done():
                fut.set_result(frame[2])

    def request(self, type_, payload=b""):
        return self.loop.run_until_complete(self._request(type_, payload))

    async def _request(self, type_, payload):
        import asyncio
        self.seq = (self.seq + 1) & 0xFF
        seq = self.seq
        fut = self.loop.create_future()
        self._pending = (seq, type_ | RESP_BIT, fut)
        try:
            # Write-with-response so large ADD frames use a GATT Long Write and we
            # know the device accepted the frame.
            await self.client.write_gatt_char(BLE_CMD_UUID, encode(type_, seq, payload), response=True)
            try:
                return await asyncio.wait_for(fut, self.timeout)
            except asyncio.TimeoutError:
                raise TimeoutError(f"no response to opcode 0x{type_:02x}") from None
        finally:
            self._pending = None

    def close(self, disconnect=False):
        # A trusted esp-otp is a HID keyboard, so BlueZ's input plugin holds the
        # ACL link open. Calling disconnect() here just drops it and BlueZ
        # instantly reconnects, popping a "disconnected"+"reconnected" pair on
        # every run. So by default we only drop our GATT notify subscription and
        # leave the link to BlueZ. Pass disconnect=True (--ble-disconnect) to
        # fully tear it down, e.g. for an unbonded one-off.
        try:
            self.loop.run_until_complete(self.client.stop_notify(BLE_RSP_UUID))
        except Exception:
            pass
        if disconnect:
            try:
                self.loop.run_until_complete(self.client.disconnect())
            except Exception:
                pass
        self.loop.close()


def check_status(payload):
    st = payload[0]
    if st != 0:
        sys.exit(f"device error: {STATUS.get(st, st)}")
    return payload


def cmd_info(dev, _):
    p = check_status(dev.request(OP_GET_INFO))
    major, minor, patch, flags = p[1], p[2], p[3], p[4]
    count, cap = struct.unpack("<HH", p[5:9])
    print(f"fw {major}.{minor}.{patch}")
    print(f"rtc_present={bool(flags & 1)} time_valid={bool(flags & 2)}")
    print(f"secrets {count}/{cap}")
    if len(p) >= 10:  # name_len + name appended (older fw omits it)
        nlen = p[9]
        print(f"name {p[10:10 + nlen].decode('utf-8', 'replace')!r}")


def cmd_screen(dev, args):
    # Pull the framebuffer in MTU-sized slices until we have `total` bytes.
    start, total, width, pages, fb = 0, None, 0, 0, bytearray()
    while True:
        p = check_status(dev.request(OP_GET_SCREEN, struct.pack("<H", start)))
        total, width, pages = struct.unpack("<H", p[1:3])[0], p[3], p[4]
        chunk = p[5:]
        if not chunk:
            break
        fb += chunk
        start += len(chunk)
        if start >= total:
            break
    if len(fb) < total:
        sys.exit(f"short screen read: got {len(fb)} of {total} bytes")

    if args.raw:
        sys.stdout.buffer.write(bytes(fb))
        return
    # Each byte is a vertical run of 8 pixels (bit0 = top); pages stack vertically.
    for y in range(pages * 8):
        row = "".join("#" if fb[(y // 8) * width + x] >> (y % 8) & 1 else " "
                      for x in range(width))
        print(row)


def cmd_settime(dev, args):
    now = int(args.epoch) if args.epoch is not None else int(time.time())
    check_status(dev.request(OP_SET_TIME, struct.pack("<I", now)))
    print(f"time set to {now}")


def cmd_list(dev, _):
    start = 0
    while True:
        p = check_status(dev.request(OP_LIST, bytes([start])))
        total = struct.unpack("<H", p[1:3])[0]
        n = p[3]
        off = 4
        for _ in range(n):
            id_, digits, period, algo, llen = struct.unpack("<HBHBB", p[off:off + 7])
            off += 7
            label = p[off:off + llen].decode("utf-8", "replace")
            off += llen
            print(f"  id={id_:<4} {label!r} {digits}d/{period}s {ALGO.get(algo, algo)}")
            start += 1
        if start >= total or n == 0:
            break
    if total == 0:
        print("  (no secrets)")


ALGO_BY_NAME = {"SHA1": 0, "SHA256": 1, "SHA512": 2}

# Base32 data alphabet (RFC 4648), case-insensitive; the device also ignores
# space, '-' and '=' as separators/padding (see totp.c base32_decode).
_B32_ALPHABET = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz234567")
_B32_SEPARATORS = set(" -=")


def check_base32(secret):
    """Validate a base32 secret the way the device will, before sending it.

    Mirrors totp.c: separators (space/-/=) are ignored, every other character
    must be in the base32 alphabet, and at least one data char is required.
    """
    data = [c for c in secret if c not in _B32_SEPARATORS]
    if not data:
        raise ValueError("empty base32 secret")
    bad = next((c for c in data if c not in _B32_ALPHABET), None)
    if bad is not None:
        raise ValueError(f"invalid base32 character {bad!r} in secret")


def parse_otpauth(uri):
    """Parse an otpauth://totp/ URI into (label, secret, digits, period, algo).

    Format (Key Uri Format):
        otpauth://totp/LABEL?secret=BASE32&issuer=I&algorithm=SHA1&digits=6&period=30
    Only TOTP is supported; HOTP or a missing secret is rejected.
    """
    from urllib.parse import urlparse, parse_qs, unquote

    u = urlparse(uri)
    if u.scheme != "otpauth":
        raise ValueError("not an otpauth:// URI")
    if u.netloc.lower() != "totp":
        raise ValueError(f"unsupported otpauth type {u.netloc!r} (only totp)")
    q = parse_qs(u.query)

    secret = (q.get("secret", [""])[0]).strip()
    if not secret:
        raise ValueError("otpauth URI has no secret")

    # Label is the URL-encoded path ("Issuer:Account" or "Account"); fall back to
    # the issuer param when the path is empty.
    label = unquote(u.path.lstrip("/")).strip()
    if not label:
        label = q.get("issuer", [""])[0].strip()

    algo_name = q.get("algorithm", ["SHA1"])[0].upper()
    if algo_name not in ALGO_BY_NAME:
        raise ValueError(f"unsupported algorithm {algo_name!r}")

    return {
        "label": label,
        "secret": secret,
        "digits": int(q.get("digits", ["6"])[0]),
        "period": int(q.get("period", ["30"])[0]),
        "algo": ALGO_BY_NAME[algo_name],
    }


def add_prepare(args):
    """Resolve and validate `add` args without touching the device, so typos and
    bad secrets fail before we even open the serial port. Returns a single-item
    list of entries. Exits with a message on any error.

    Two forms:
      add LABEL SECRET [flags]
      add otpauth://totp/...   (secret + digits/period/algo from the URI)

    Google Authenticator (otpauth-migration://) exports aren't handled here;
    convert them to plain otpauth:// URLs first with tools/otpauth_migration.py.
    """
    if args.label.startswith("otpauth://"):
        if args.secret is not None:
            sys.exit("add: pass either an otpauth:// URI or a label+secret, not both")
        try:
            f = parse_otpauth(args.label)
        except ValueError as e:
            sys.exit(f"add: {e}")
        entries = [{
            "label": f["label"], "secret": f["secret"],
            "digits": f["digits"] if args.digits is None else args.digits,
            "period": f["period"] if args.period is None else args.period,
            "algo": f["algo"] if args.algo is None else args.algo,
        }]
    else:
        if args.secret is None:
            sys.exit("add: SECRET is required (or pass a single otpauth:// URI)")
        entries = [{
            "label": args.label, "secret": args.secret,
            "digits": 6 if args.digits is None else args.digits,
            "period": 30 if args.period is None else args.period,
            "algo": 0 if args.algo is None else args.algo,
        }]

    for e in entries:
        try:
            check_base32(e["secret"])
        except ValueError as err:
            sys.exit(f"add: {e['label']!r}: {err}")
    return entries


def add_show_dry_run(entries):
    """Print what `add` would send, without revealing any secret material."""
    print(f"dry-run: {len(entries)} secret(s) would be added:")
    for e in entries:
        print(f"  {e['label']!r} {e['digits']}d/{e['period']}s "
              f"{ALGO.get(e['algo'], e['algo'])} secret=[{len(e['secret'])} base32 chars]")


def cmd_add(dev, args):
    for e in args.prepared:
        label = e["label"].encode("utf-8")
        secret = e["secret"].encode("ascii")
        payload = bytes([e["digits"]]) + struct.pack("<H", e["period"]) + bytes([e["algo"]])
        payload += bytes([len(label)]) + label + bytes([len(secret)]) + secret
        p = check_status(dev.request(OP_ADD, payload))
        print(f"added id={struct.unpack('<H', p[1:3])[0]} {e['label']!r} "
              f"{e['digits']}d/{e['period']}s {ALGO.get(e['algo'], e['algo'])}")


def cmd_remove(dev, args):
    check_status(dev.request(OP_REMOVE, struct.pack("<H", args.id)))
    print("removed")


def cmd_rename(dev, args):
    label = args.label.encode("utf-8")
    check_status(dev.request(OP_RENAME, struct.pack("<H", args.id) + bytes([len(label)]) + label))
    print("renamed")


def cmd_move(dev, args):
    check_status(dev.request(OP_MOVE, struct.pack("<HB", args.id, args.index)))
    print(f"moved id {args.id} to index {args.index}")


def cmd_setname(dev, args):
    name = args.new_name.encode("utf-8")
    if not 1 <= len(name) <= 20:
        sys.exit("name must be 1..20 bytes (UTF-8)")
    check_status(dev.request(OP_SET_NAME, bytes([len(name)]) + name))
    print(f"device name set to {args.new_name!r}")


def cmd_button(dev, args):
    check_status(dev.request(OP_INJECT_BUTTON, bytes([BUTTON[args.gesture]])))
    print(f"injected {args.gesture} press")


def cmd_reboot(dev, _):
    check_status(dev.request(OP_REBOOT))
    print("reboot scheduled")


def cmd_ping(dev, _):
    check_status(dev.request(OP_PING))
    print("pong")


def selftest():
    assert crc16(b"123456789") == 0x29B1, "CRC vector mismatch"
    # encode/parse round-trip
    f = encode(OP_ADD, 0x11, bytes([0xDE, 0xAD, 0xBE, 0xEF, 0x42]))
    p = Parser()
    out = None
    for b in f:
        out = p.push(b) or out
    assert out == (OP_ADD, 0x11, bytes([0xDE, 0xAD, 0xBE, 0xEF, 0x42])), out
    # empty payload
    f = encode(OP_PING, 1)
    p = Parser()
    out = None
    for b in f:
        out = p.push(b) or out
    assert out == (OP_PING, 1, b""), out
    # otpauth URI parsing: label from path, params override defaults
    f = parse_otpauth(
        "otpauth://totp/GitHub:me%40x.com?secret=JBSWY3DPEHPK3PXP"
        "&issuer=GitHub&algorithm=SHA256&digits=8&period=60")
    assert f == {"label": "GitHub:me@x.com", "secret": "JBSWY3DPEHPK3PXP",
                 "digits": 8, "period": 60, "algo": 1}, f
    # defaults when params absent; label falls back to issuer
    f = parse_otpauth("otpauth://totp/?secret=ABCD&issuer=ACME")
    assert f == {"label": "ACME", "secret": "ABCD",
                 "digits": 6, "period": 30, "algo": 0}, f
    for bad in ("otpauth://hotp/x?secret=A", "otpauth://totp/x", "https://x"):
        try:
            parse_otpauth(bad)
        except ValueError:
            pass
        else:
            raise AssertionError(f"expected reject: {bad}")
    # base32 pre-check: accept alphabet + separators, reject 0/1/8/9 and symbols
    for ok in ("JBSWY3DPEHPK3PXP", "jbsw y3dp ee======", "GEZD-GNBV"):
        check_base32(ok)
    for bad in ("", "   ", "ABC0", "hello!", "18990"):
        try:
            check_base32(bad)
        except ValueError:
            pass
        else:
            raise AssertionError(f"expected base32 reject: {bad!r}")
    print("host_cli selftest OK")
    print("ping frame:", encode(OP_PING, 1).hex())


def main():
    ap = argparse.ArgumentParser(description="esp-otp host CLI")
    ap.add_argument("-p", "--port", help="serial port, e.g. /dev/ttyACM0")
    ap.add_argument("--ble", action="store_true",
                    help="use the BLE management service instead of a serial port (needs bleak)")
    ap.add_argument("--name", default=None,
                    help="optional GAP name to narrow the BLE match (default: match "
                         "any esp-otp by service UUID; --ble only)")
    ap.add_argument("--address", default=None,
                    help="exact BLE address to match, overrides --name (--ble only)")
    ap.add_argument("--ble-disconnect", action="store_true",
                    help="fully tear down the BLE link on exit (default: leave it to "
                         "BlueZ; dropping a trusted keyboard's link makes it reconnect)")
    ap.add_argument("--selftest", action="store_true", help="run framing self-test and exit")
    sub = ap.add_subparsers(dest="cmd")

    sub.add_parser("ping").set_defaults(func=cmd_ping)
    sub.add_parser("info").set_defaults(func=cmd_info)
    st = sub.add_parser("settime")
    st.add_argument("epoch", nargs="?", default=None, help="unix seconds (default: now)")
    st.set_defaults(func=cmd_settime)
    sub.add_parser("list").set_defaults(func=cmd_list)
    sc = sub.add_parser("screen", help="dump the OLED contents (ASCII, or --raw bytes)")
    sc.add_argument("--raw", action="store_true", help="write the raw framebuffer to stdout")
    sc.set_defaults(func=cmd_screen)
    add = sub.add_parser("add", help="add a secret from label+secret or an otpauth:// URI")
    add.add_argument("label", help="account label, OR a full otpauth://totp/... URI")
    add.add_argument("secret", nargs="?", default=None,
                     help="base32 secret (omit when label is an otpauth:// URI)")
    add.add_argument("--digits", type=int, default=None, help="default 6 (or from URI)")
    add.add_argument("--period", type=int, default=None, help="default 30 (or from URI)")
    add.add_argument("--algo", type=int, default=None,
                     help="0=SHA1 1=SHA256 2=SHA512 (default 0, or from URI)")
    add.add_argument("--dry-run", action="store_true",
                     help="show what would be added (no secrets, no device I/O) and exit")
    add.set_defaults(func=cmd_add, prepare=add_prepare)
    rm = sub.add_parser("remove")
    rm.add_argument("id", type=int)
    rm.set_defaults(func=cmd_remove)
    rn = sub.add_parser("rename")
    rn.add_argument("id", type=int)
    rn.add_argument("label")
    rn.set_defaults(func=cmd_rename)
    mv = sub.add_parser("move", help="reorder a token to a new list position")
    mv.add_argument("id", type=int)
    mv.add_argument("index", type=int, help="0-based target position (0..count-1)")
    mv.set_defaults(func=cmd_move)
    sn = sub.add_parser("setname", help="change the BLE advertised device name")
    # dest new_name (not "name") so it doesn't clobber the global --name selector.
    sn.add_argument("new_name", metavar="name", help="new device name (1..20 bytes UTF-8)")
    sn.set_defaults(func=cmd_setname)
    bt = sub.add_parser("button", help="inject a button press (drives the on-device UI)")
    bt.add_argument("gesture", choices=list(BUTTON),
                    help="single=cycle view, double=cycle HID target, long=pair/type code")
    bt.set_defaults(func=cmd_button)
    sub.add_parser("reboot", help="restart the device").set_defaults(func=cmd_reboot)

    args = ap.parse_args()
    if args.selftest:
        selftest()
        return
    if not args.cmd:
        ap.error("a command is required (or use --selftest)")

    # Validate args that need no device first, so typos fail before we open the
    # port (and before any bytes hit the wire).
    prepare = getattr(args, "prepare", None)
    args.prepared = prepare(args) if prepare else None

    # A dry run just reports the prepared plan and never touches the device.
    if getattr(args, "dry_run", False):
        add_show_dry_run(args.prepared)
        return

    # Transport: explicit flags win; otherwise fall back to the last successful
    # connection saved in the shared config (same file tui.py uses).
    use_ble, port, name, address = args.ble, args.port, args.name, args.address
    if not use_ble and not port:
        last = load_last_conn()
        if last is None:
            ap.error("a transport is required: -p/--port for serial, or --ble "
                     "(no saved connection to fall back on)")
        if last["kind"] == "ble":
            use_ble, name = True, name or (last["target"] or None)
        else:
            port = last["target"]
        print(f"using last connection: {last['kind']} {last['target'] or '(any)'}",
              file=sys.stderr)

    try:
        if use_ble:
            dev = BleDevice(name=name, address=address)
            save_last_conn("ble", address or name or "")
        else:
            dev = Device(port)
            save_last_conn("serial", port)
    except Exception as e:  # TimeoutError, serial/OSError, bleak BleakError, …
        sys.exit(f"connection failed: {e}")

    try:
        args.func(dev, args)
    finally:
        if isinstance(dev, BleDevice):
            dev.close(disconnect=args.ble_disconnect)


if __name__ == "__main__":
    main()
