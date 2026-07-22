#!/usr/bin/env python3
"""esp-otp Textual TUI — a terminal console with the same capabilities as the
web page (tools/web): connect over BLE or serial, mirror the OLED live, drive the
button (single/double/long), reboot, and manage secrets (list/add/remove/rename/move)
plus set-time and ping.

Mouse-driven throughout (Textual): click buttons, select table rows, focus
inputs. Keyboard works too.

    tools/.venv/bin/python tools/tui.py           # BLE, finds any esp-otp by service UUID

Reuses the wire codec and helpers from host_cli.py; only the transport is made
async so it rides Textual's event loop.
"""
import asyncio
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from host_cli import (  # noqa: E402  (after sys.path tweak)
    ALGO, BLE_CMD_UUID, BLE_RSP_UUID, BUTTON, OP_ADD, OP_GET_INFO,
    OP_GET_SCREEN, OP_INJECT_BUTTON, OP_LIST, OP_MOVE, OP_PING, OP_REBOOT,
    OP_REMOVE, OP_RENAME, OP_SET_NAME, OP_SET_TIME, RESP_BIT, STATUS, Parser,
    check_base32, choose_one_esp_otp, encode, load_last_conn, parse_otpauth,
    save_last_conn, scan_esp_otp,
)

from rich.text import Text  # noqa: E402
from textual import work  # noqa: E402
from textual.app import App, ComposeResult  # noqa: E402
from textual.containers import Horizontal, Vertical, VerticalScroll  # noqa: E402
from textual.screen import ModalScreen  # noqa: E402
from textual.widgets import (  # noqa: E402
    Button, DataTable, Footer, Header, Input, Label, RadioButton, RadioSet,
    RichLog, Select, Static,
)

OLED_ON = "#39ff8c"
OLED_OFF = "#04140c"


class DeviceError(Exception):
    """A command returned a non-OK status byte."""


# --------------------------------------------------------------- transports
# Each transport calls self.on_bytes(data) with received bytes and self.on_gone()
# when the link drops. send() writes one framed request.

class BleTransport:
    kind = "ble"

    def __init__(self):
        self.on_bytes = None
        self.on_gone = None
        self.client = None

    async def connect(self, target, scan_timeout=10.0):
        from bleak import BleakClient, BleakScanner
        target = target.strip()
        if ":" in target:
            dev = await BleakScanner.find_device_by_address(target, timeout=scan_timeout)
            if dev is None:
                raise TimeoutError(f"esp-otp not found ({target})")
        else:
            # Identify by the advertised management service UUID (rename-proof); a
            # non-empty target narrows by GAP name. Fail loudly on ambiguity.
            dev = choose_one_esp_otp(
                await scan_esp_otp(target or None, scan_timeout), target or None)
        self.client = BleakClient(dev, disconnected_callback=self._disc)
        await self.client.connect()
        await self.client.start_notify(BLE_RSP_UUID, self._notify)

    def _notify(self, _char, data):
        if self.on_bytes:
            self.on_bytes(bytes(data))

    def _disc(self, _client):
        if self.on_gone:
            self.on_gone()

    async def send(self, frame):
        await self.client.write_gatt_char(BLE_CMD_UUID, frame, response=True)

    async def disconnect(self):
        # Leave the ACL link to BlueZ (the device is a trusted HID keyboard;
        # dropping it makes BlueZ reconnect and flap). Just drop our notify sub.
        try:
            if self.client:
                await self.client.stop_notify(BLE_RSP_UUID)
        except Exception:
            pass


class SerialTransport:
    kind = "serial"

    def __init__(self):
        self.on_bytes = None
        self.on_gone = None
        self.ser = None
        self._stop = False
        self._task = None

    async def connect(self, target, baud=115200):
        import serial
        loop = asyncio.get_running_loop()
        self.ser = await loop.run_in_executor(
            None, lambda: serial.Serial(target, baud, timeout=0.1))
        self._task = asyncio.create_task(self._reader(loop))

    async def _reader(self, loop):
        while not self._stop:
            try:
                data = await loop.run_in_executor(None, self.ser.read, 256)
            except Exception:
                break
            if data and self.on_bytes:
                self.on_bytes(data)
        if self.on_gone:
            self.on_gone()

    async def send(self, frame):
        loop = asyncio.get_running_loop()
        await loop.run_in_executor(None, self.ser.write, frame)

    async def disconnect(self):
        self._stop = True
        if self._task:
            self._task.cancel()
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass


# ----------------------------------------------------------- async client
class Device:
    """Async management client: same opcodes as host_cli, request/response keyed
    by seq. A lock serialises requests so overlapping writes never collide."""

    def __init__(self, transport):
        self.t = transport
        self.seq = 0
        self.parser = Parser()
        self.pending = {}
        self.lock = asyncio.Lock()
        transport.on_bytes = self._feed

    def _feed(self, data):
        for b in data:
            frame = self.parser.push(b)
            if not frame:
                continue
            type_, seq, payload = frame
            if not (type_ & RESP_BIT):
                continue                       # ignore anything that isn't a response
            fut = self.pending.get(seq)
            if fut and not fut.done():
                self.pending.pop(seq, None)
                fut.set_result(payload)

    async def request(self, op, payload=b"", timeout=3.0):
        async with self.lock:
            self.seq = (self.seq + 1) & 0xFF
            seq = self.seq
            fut = asyncio.get_running_loop().create_future()
            self.pending[seq] = fut
            await self.t.send(encode(op, seq, payload))
            try:
                return await asyncio.wait_for(fut, timeout)
            except asyncio.TimeoutError:
                self.pending.pop(seq, None)
                raise TimeoutError(f"no response to opcode 0x{op:02x}") from None

    async def req(self, op, payload=b""):
        p = await self.request(op, payload)
        if not p or p[0] != 0:
            raise DeviceError(STATUS.get(p[0] if p else -1, p[0] if p else "empty"))
        return p

    async def info(self):
        p = await self.req(OP_GET_INFO)
        count, cap = struct.unpack("<HH", p[5:9])
        name = ""
        if len(p) >= 10:  # name_len + name appended (older fw omits it)
            name = p[10:10 + p[9]].decode("utf-8", "replace")
        return {"fw": f"{p[1]}.{p[2]}.{p[3]}", "rtc": bool(p[4] & 1),
                "time_valid": bool(p[4] & 2), "count": count, "cap": cap,
                "name": name}

    async def list_secrets(self):
        start, out, total = 0, [], 0
        while True:
            p = await self.req(OP_LIST, bytes([start]))
            total = struct.unpack("<H", p[1:3])[0]
            n = p[3]
            off = 4
            for _ in range(n):
                id_, digits, period, algo, llen = struct.unpack("<HBHBB", p[off:off + 7])
                off += 7
                label = p[off:off + llen].decode("utf-8", "replace")
                off += llen
                out.append((id_, label, digits, period, algo))
                start += 1
            if start >= total or n == 0:
                break
        return out

    async def get_screen(self):
        start, total, width, pages, fb = 0, 0, 0, 0, bytearray()
        while True:
            p = await self.req(OP_GET_SCREEN, struct.pack("<H", start))
            total = struct.unpack("<H", p[1:3])[0]
            width, pages = p[3], p[4]
            chunk = p[5:]
            if not chunk:
                break
            fb += chunk
            start += len(chunk)
            if start >= total:
                break
        return width, pages, bytes(fb)

    async def set_time(self):
        await self.req(OP_SET_TIME, struct.pack("<I", int(time.time())))

    async def add(self, label, secret, digits, period, algo):
        lb = label.encode("utf-8")
        sb = secret.encode("ascii")
        payload = bytes([digits]) + struct.pack("<H", period) + bytes([algo])
        payload += bytes([len(lb)]) + lb + bytes([len(sb)]) + sb
        p = await self.req(OP_ADD, payload)
        return struct.unpack("<H", p[1:3])[0]

    async def remove(self, id_):
        await self.req(OP_REMOVE, struct.pack("<H", id_))

    async def rename(self, id_, label):
        lb = label.encode("utf-8")
        await self.req(OP_RENAME, struct.pack("<H", id_) + bytes([len(lb)]) + lb)

    async def move(self, id_, index):
        await self.req(OP_MOVE, struct.pack("<HB", id_, index))

    async def button(self, gesture):
        await self.req(OP_INJECT_BUTTON, bytes([BUTTON[gesture]]))

    async def set_name(self, name):
        nb = name.encode("utf-8")
        await self.req(OP_SET_NAME, bytes([len(nb)]) + nb)

    async def reboot(self):
        await self.req(OP_REBOOT)

    async def ping(self):
        await self.req(OP_PING)


def _sextant(v):
    """Map a 6-bit 2x3 cell pattern to its Unicode block-sextant glyph. The
    legacy-computing block (U+1FB00..) omits blank/left-half/right-half/full,
    which reuse existing block chars, so those four are special-cased."""
    if v == 0:
        return " "
    if v == 0b010101:      # left column -> LEFT HALF BLOCK
        return "▌"
    if v == 0b101010:      # right column -> RIGHT HALF BLOCK
        return "▐"
    if v == 0b111111:      # all -> FULL BLOCK
        return "█"
    idx = v - 1 - (v > 0b010101) - (v > 0b101010)
    return chr(0x1FB00 + idx)


def render_fb(width, pages, fb):
    """Framebuffer -> Rich Text using block sextants (2x3 solid pixels per cell),
    so a 72x40 panel packs into 36x14 characters with a filled, screen-like look.
    Bit weights within a cell: 1<<(dy*2 + dx) for dx in 0..1, dy in 0..2."""
    h = pages * 8

    def px(x, y):
        return (fb[(y // 8) * width + x] >> (y % 8) & 1) if (0 <= x < width and 0 <= y < h) else 0

    style = f"{OLED_ON} on {OLED_OFF}"
    rows, cols = (h + 2) // 3, (width + 1) // 2
    t = Text()
    for cy in range(rows):
        for cx in range(cols):
            v = 0
            for dy in range(3):
                for dx in range(2):
                    if px(cx * 2 + dx, cy * 3 + dy):
                        v |= 1 << (dy * 2 + dx)
            t.append(_sextant(v), style=style)
        if cy + 1 < rows:
            t.append("\n")
    return t


# ------------------------------------------------------------------ modals
class ConfirmScreen(ModalScreen[bool]):
    def __init__(self, prompt):
        super().__init__()
        self.prompt = prompt

    def compose(self):
        with Vertical(id="modal"):
            yield Label(self.prompt)
            with Horizontal():
                yield Button("Yes", variant="error", id="yes")
                yield Button("No", id="no")

    def on_button_pressed(self, event):
        self.dismiss(event.button.id == "yes")


class PromptScreen(ModalScreen[str]):
    def __init__(self, prompt, value=""):
        super().__init__()
        self.prompt = prompt
        self.value = value

    def compose(self):
        with Vertical(id="modal"):
            yield Label(self.prompt)
            yield Input(value=self.value, id="prompt-input")
            with Horizontal():
                yield Button("OK", variant="primary", id="ok")
                yield Button("Cancel", id="cancel")

    def on_button_pressed(self, event):
        self.dismiss(self.query_one("#prompt-input", Input).value if event.button.id == "ok" else None)

    def on_input_submitted(self, event):
        self.dismiss(event.value)


# --------------------------------------------------------------------- app
class EspOtpApp(App):
    TITLE = "esp-otp console"
    CSS = """
    #bar { height: auto; padding: 0 1; }
    #bar RadioSet { width: 14; height: auto; border: none; }
    #bar Input { width: 30; }
    #main { height: 1fr; }
    #left { width: 48; height: 1fr; }
    #right { width: 1fr; height: 1fr; }
    .panel { border: round $primary; padding: 0 1; margin: 1 0 0 0; }
    .panel > Label { color: $text-muted; text-style: bold; }
    #devicebox { height: 9; }
    #displaybox { height: 22; }
    #secretsbox { height: 19; }
    #addbox { height: 22; }
    #oled { width: 38; height: 16; background: #04140c; border: round $panel-darken-1; }
    #info { height: auto; }
    #secrets { height: 12; }
    #log { height: 8; border: round $panel; }
    Button { margin: 0 1 0 0; min-width: 8; }
    #modal { background: $panel; border: thick $primary; padding: 1 2; width: 60; height: auto; }
    .status-on { color: $success; text-style: bold; }
    .status-off { color: $text-muted; }
    .status-err { color: $error; text-style: bold; }
    """
    BINDINGS = [("q", "quit", "Quit")]

    def __init__(self):
        super().__init__()
        self.device = None
        self.transport = None
        self.transport_kind = "ble"
        self.selected_id = None
        self.selected_index = None
        self._device_name = ""
        self._screen_busy = False
        self._screen_timer = None

    def compose(self) -> ComposeResult:
        yield Header()
        with Horizontal(id="bar"):
            with RadioSet(id="transport"):
                yield RadioButton("BLE", value=True, id="rb-ble")
                yield RadioButton("Serial", id="rb-serial")
            yield Input(placeholder="blank = any esp-otp / name / address / port",
                        id="target")
            yield Button("Connect", variant="primary", id="connect")
            yield Label("disconnected", id="status", classes="status-off")
        with Horizontal(id="main"):
            with VerticalScroll(id="left"):
                with Vertical(classes="panel", id="devicebox"):
                    yield Label("Device")
                    yield Static("not connected", id="info")
                    with Horizontal():
                        yield Button("Ping", id="ping", disabled=True)
                        yield Button("Refresh", id="refresh", disabled=True)
                        yield Button("Set time", id="settime", disabled=True)
                        yield Button("Rename dev", id="setname", disabled=True)
                with Vertical(classes="panel", id="displaybox"):
                    yield Label("Display  (live 1 Hz)")
                    yield Static(id="oled")
                    with Horizontal():
                        yield Button("Single", id="btn-single", disabled=True)
                        yield Button("Double", id="btn-double", disabled=True)
                        yield Button("Long", id="btn-long", disabled=True)
                        yield Button("Reset", variant="error", id="btn-reboot", disabled=True)
            with VerticalScroll(id="right"):
                with Vertical(classes="panel", id="secretsbox"):
                    yield Label("Secrets")
                    yield DataTable(id="secrets", cursor_type="row")
                    with Horizontal():
                        yield Button("Up", id="move-up", disabled=True)
                        yield Button("Down", id="move-down", disabled=True)
                        yield Button("Rename", id="rename", disabled=True)
                        yield Button("Remove", variant="error", id="remove", disabled=True)
                with Vertical(classes="panel", id="addbox"):
                    yield Label("Add secret")
                    yield Input(placeholder="otpauth://totp/… (then Fill)", id="url")
                    with Horizontal():
                        yield Button("Fill from URL", id="fill-url")
                    yield Input(placeholder="label", id="a-label")
                    yield Input(placeholder="base32 secret", id="a-secret")
                    with Horizontal():
                        yield Input(value="6", placeholder="digits", id="a-digits")
                        yield Input(value="30", placeholder="period", id="a-period")
                        yield Select([("SHA1", 0), ("SHA256", 1), ("SHA512", 2)],
                                     value=0, allow_blank=False, id="a-algo")
                    with Horizontal():
                        yield Button("Add", variant="primary", id="add", disabled=True)
        yield RichLog(id="log", markup=True)
        yield Footer()

    def on_mount(self):
        self.query_one("#secrets", DataTable).add_columns("id", "label", "type")
        last = load_last_conn()
        if last:
            self.transport_kind = last["kind"]
            if last["kind"] == "serial":
                self.query_one("#rb-serial", RadioButton).value = True
            # RadioSet.Changed (from the line above) resets the target to a
            # default, so set the saved target after and reconnect.
            self.call_after_refresh(self._reconnect_last, last["target"])
        else:
            self.log_line("ready — pick a transport and Connect")

    def _reconnect_last(self, target):
        self.query_one("#target", Input).value = target
        self.log_line(f"reconnecting to last device ({self.transport_kind}) {target!r}")
        self.toggle_connect()

    # ------------------------------------------------------------- helpers
    def log_line(self, msg, err=False):
        style = "[red]" if err else ""
        self.query_one("#log", RichLog).write(
            f"{style}{time.strftime('%H:%M:%S')}  {msg}")

    def set_status(self, text, cls):
        s = self.query_one("#status", Label)
        s.update(text)
        s.set_classes(cls)

    def enable_controls(self, on):
        for wid in ("ping", "refresh", "settime", "setname", "btn-single",
                    "btn-double", "btn-long", "btn-reboot", "move-up",
                    "move-down", "rename", "remove", "add"):
            self.query_one("#" + wid, Button).disabled = not on

    # ------------------------------------------------------------- events
    def on_radio_set_changed(self, event: RadioSet.Changed):
        self.transport_kind = "serial" if event.pressed.id == "rb-serial" else "ble"
        target = self.query_one("#target", Input)
        # BLE: blank target matches any esp-otp by service UUID (rename-proof).
        target.value = "/dev/ttyACM0" if self.transport_kind == "serial" else ""

    def on_data_table_row_selected(self, event: DataTable.RowSelected):
        try:
            self.selected_id = int(event.row_key.value)
            self.selected_index = event.cursor_row
        except (TypeError, ValueError):
            self.selected_id = None
            self.selected_index = None

    def on_button_pressed(self, event: Button.Pressed):
        bid = event.button.id
        if bid == "connect":
            self.toggle_connect()
        elif bid == "ping":
            self.run_op(self._ping())
        elif bid == "refresh":
            self.run_op(self.reload_device())
        elif bid == "settime":
            self.run_op(self._settime())
        elif bid == "setname":
            self._setname()
        elif bid in ("btn-single", "btn-double", "btn-long"):
            self.run_op(self._button(bid.split("-")[1]))
        elif bid == "btn-reboot":
            self._reboot()
        elif bid == "fill-url":
            self.fill_from_url()
        elif bid == "add":
            self.run_op(self._add())
        elif bid == "remove":
            self._remove()
        elif bid == "rename":
            self._rename()
        elif bid == "move-up":
            self._move(-1)
        elif bid == "move-down":
            self._move(+1)

    # --------------------------------------------------------- operations
    def run_op(self, coro):
        """Fire-and-forget a device op as a worker, logging any error."""
        async def wrap():
            try:
                await coro
            except Exception as e:
                self.log_line(f"{type(e).__name__}: {e}", err=True)
        self.run_worker(wrap(), exclusive=False)

    @work(exclusive=True)
    async def toggle_connect(self):
        if self.transport:
            await self.disconnect()
            return
        target = self.query_one("#target", Input).value.strip()
        self.set_status("connecting…", "status-off")
        self.log_line(f"connecting ({self.transport_kind}) to {target!r}")
        try:
            self.transport = BleTransport() if self.transport_kind == "ble" else SerialTransport()
            # on_gone fires on the event-loop thread (bleak BlueZ callback / serial
            # reader task), so schedule the UI reset with call_later, not call_from_thread.
            self.transport.on_gone = lambda: self.call_later(self._on_gone)
            self.device = Device(self.transport)
            await self.transport.connect(target)
        except Exception as e:
            self.log_line(f"connect failed: {e}", err=True)
            self.set_status("error", "status-err")
            self.transport = self.device = None
            return
        self.query_one("#connect", Button).label = "Disconnect"
        self.set_status(f"connected ({self.transport_kind})", "status-on")
        self.enable_controls(True)
        self.log_line("connected")
        save_last_conn(self.transport_kind, target)
        self._screen_timer = self.set_interval(1.0, self._tick_screen)
        await self.reload_device()

    async def disconnect(self):
        if self._screen_timer:
            self._screen_timer.stop()
            self._screen_timer = None
        try:
            if self.transport:
                await self.transport.disconnect()
        except Exception:
            pass
        self._reset_ui()
        self.log_line("disconnected")

    async def action_quit(self):
        # Quit without flapping the BLE link. bleak arms a Device1.Disconnect on an
        # AsyncExitStack that fires when its connection task is *cancelled*; a normal
        # Textual quit runs asyncio.run()'s teardown, which cancels that task, sends
        # Disconnect, and BlueZ then drops + auto-reconnects the trusted HID keyboard
        # (a disconnect/connect notification on every exit). host_cli avoids this by
        # closing its loop abruptly and never cancelling the task. We do the same:
        # drop our notify sub, restore the terminal, then hard-exit so the bleak task
        # is never cancelled. (Serial has no such teardown, so close it normally.)
        if self._screen_timer:
            self._screen_timer.stop()
            self._screen_timer = None
        if self.transport:
            try:
                await self.transport.disconnect()
            except Exception:
                pass
        hard_exit = isinstance(self.transport, BleTransport)
        self.transport = self.device = None
        if hard_exit:
            if self._driver is not None:
                self._driver.stop_application_mode()   # leave alt-screen, restore cursor
            os._exit(0)
        self.exit()

    def _on_gone(self):
        if self.transport is not None:
            self.log_line("link dropped")
            self._reset_ui()

    def _reset_ui(self):
        if self._screen_timer:
            self._screen_timer.stop()
            self._screen_timer = None
        self.transport = self.device = None
        self.selected_id = None
        self.selected_index = None
        self.query_one("#connect", Button).label = "Connect"
        self.set_status("disconnected", "status-off")
        self.enable_controls(False)
        self.query_one("#info", Static).update("not connected")
        self.query_one("#oled", Static).update("")
        self.query_one("#secrets", DataTable).clear()

    async def reload_device(self):
        if not self.device:
            return
        info = await self.device.info()
        self._device_name = info["name"]
        name_line = f"name {info['name']}\n" if info["name"] else ""
        self.query_one("#info", Static).update(
            f"{name_line}"
            f"fw {info['fw']}   rtc={'yes' if info['rtc'] else 'no'}   "
            f"time_valid={'yes' if info['time_valid'] else 'no'}\n"
            f"secrets {info['count']} / {info['cap']}")
        table = self.query_one("#secrets", DataTable)
        table.clear()
        for id_, label, digits, period, algo in await self.device.list_secrets():
            table.add_row(str(id_), label,
                          f"{digits}d/{period}s {ALGO.get(algo, algo)}", key=str(id_))

    def _tick_screen(self):
        if self.device and not self._screen_busy:
            self.run_worker(self._poll_screen(), exclusive=True, group="screen")

    async def _poll_screen(self):
        self._screen_busy = True
        try:
            width, pages, fb = await self.device.get_screen()
            self.query_one("#oled", Static).update(render_fb(width, pages, fb))
        except Exception as e:
            self.log_line(f"screen: {e}", err=True)
        finally:
            self._screen_busy = False

    async def _ping(self):
        await self.device.ping()
        self.log_line("pong")

    async def _settime(self):
        await self.device.set_time()
        self.log_line("time set to now")
        await self.reload_device()

    async def _button(self, name):
        await self.device.button(name)
        self.log_line(f"button {name}")

    @work
    async def _setname(self):
        if not self.device:
            return
        name = await self.push_screen_wait(
            PromptScreen("New BLE device name (1..20 chars):", self._device_name))
        if not name:
            return
        name = name.strip()
        if not 1 <= len(name.encode("utf-8")) <= 20:
            self.log_line("name must be 1..20 bytes", err=True)
            return
        try:
            await self.device.set_name(name)
            self.log_line(f"device name set to {name!r}")
            await self.reload_device()
            # No target fix-up needed: BLE matches by service UUID, not name, so a
            # rename never breaks reconnect (unless the target pins a specific name,
            # which the user chose deliberately).
        except Exception as e:
            self.log_line(f"set name failed: {e}", err=True)

    @work
    async def _reboot(self):
        if not self.device:
            return
        if await self.push_screen_wait(ConfirmScreen("Reboot the device? The link will drop.")):
            try:
                await self.device.reboot()
                self.log_line("reboot scheduled")
            except Exception as e:
                self.log_line(f"reboot failed: {e}", err=True)

    @work
    async def _remove(self):
        if not self.device or self.selected_id is None:
            self.log_line("select a secret first", err=True)
            return
        if await self.push_screen_wait(ConfirmScreen(f"Remove secret id {self.selected_id}?")):
            try:
                await self.device.remove(self.selected_id)
                self.log_line(f"removed id={self.selected_id}")
                await self.reload_device()
            except Exception as e:
                self.log_line(f"remove failed: {e}", err=True)

    @work
    async def _rename(self):
        if not self.device or self.selected_id is None:
            self.log_line("select a secret first", err=True)
            return
        label = await self.push_screen_wait(PromptScreen(f"New label for id {self.selected_id}:"))
        if label:
            try:
                await self.device.rename(self.selected_id, label)
                self.log_line(f"renamed id={self.selected_id}")
                await self.reload_device()
            except Exception as e:
                self.log_line(f"rename failed: {e}", err=True)

    @work
    async def _move(self, delta):
        if not self.device or self.selected_id is None or self.selected_index is None:
            self.log_line("select a secret first", err=True)
            return
        count = self.query_one("#secrets", DataTable).row_count
        new_index = self.selected_index + delta
        if new_index < 0 or new_index >= count:
            return  # already at an edge
        try:
            await self.device.move(self.selected_id, new_index)
            self.log_line(f"moved id={self.selected_id} to index {new_index}")
            await self.reload_device()
        except Exception as e:
            self.log_line(f"move failed: {e}", err=True)

    def fill_from_url(self):
        raw = self.query_one("#url", Input).value.strip()
        try:
            f = parse_otpauth(raw)
        except ValueError as e:
            self.log_line(f"URL parse failed: {e}", err=True)
            return
        self.query_one("#a-label", Input).value = f["label"][:31]
        self.query_one("#a-secret", Input).value = f["secret"]
        self.query_one("#a-digits", Input).value = str(f["digits"])
        self.query_one("#a-period", Input).value = str(f["period"])
        self.query_one("#a-algo", Select).value = f["algo"]
        self.log_line(f"parsed otpauth for {f['label']!r}")

    async def _add(self):
        label = self.query_one("#a-label", Input).value.strip()
        secret = self.query_one("#a-secret", Input).value.replace(" ", "").upper()
        if not label:
            self.log_line("add: label required", err=True)
            return
        try:
            check_base32(secret)
            digits = int(self.query_one("#a-digits", Input).value)
            period = int(self.query_one("#a-period", Input).value)
        except ValueError as e:
            self.log_line(f"add: {e}", err=True)
            return
        algo = self.query_one("#a-algo", Select).value
        id_ = await self.device.add(label, secret, digits, period, algo)
        self.log_line(f"added id={id_} {label!r}")
        for wid in ("url", "a-label", "a-secret"):
            self.query_one("#" + wid, Input).value = ""
        await self.reload_device()


def main():
    EspOtpApp().run()


if __name__ == "__main__":
    main()
