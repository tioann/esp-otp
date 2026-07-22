"use strict";
// esp-otp browser console. One management protocol (PROTOCOL.md) spoken over
// two transports: Web Serial or Web Bluetooth to a real device. The protocol
// codec below is the JS twin of tools/host_cli.py; keep them in sync.

// ----------------------------------------------------------------- frame codec
const SOF = 0x7e;
const RESP_BIT = 0x80;
const OP = { PING: 1, INFO: 2, SETTIME: 3, LIST: 4, ADD: 5, REMOVE: 6, RENAME: 7,
             SCREEN: 8, BUTTON: 9, REBOOT: 0x0a, MOVE: 0x0b, SETNAME: 0x0c };
const BUTTON = { single: 1, double: 2, long: 3 };  // INJECT_BUTTON gesture codes
const STATUS = ["OK", "BAD_REQUEST", "FULL", "NOT_FOUND", "INVALID_ARG",
                "NOT_AUTHORIZED", "CRC_ERROR", "UNSUPPORTED"];
const ALGO = ["SHA1", "SHA256", "SHA512"];

function crc16(data) {            // CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF)
  let crc = 0xffff;
  for (const b of data) {
    crc ^= b << 8;
    for (let i = 0; i < 8; i++)
      crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
  }
  return crc;
}

// Encode a management body ([type,seq,...payload]) into a 0x7E|LEN|body|CRC frame.
function frame(body) {
  const len = body.length;
  const crc = crc16(body);
  return Uint8Array.from([SOF, len & 0xff, (len >> 8) & 0xff, ...body,
                          crc & 0xff, (crc >> 8) & 0xff]);
}
const encodeMgmt = (type, seq, payload = []) => frame([type, seq, ...payload]);

// Streaming frame extractor for management frames (body >= 2: type+seq);
// tolerates noise (log text, ROM boot chatter) between frames.
class Parser {
  constructor(minBody) { this.min = minBody; this.reset(); }
  reset() { this.state = 0; this.len = 0; this.buf = []; }
  // Feed one byte; return the body bytes (Uint8Array) on a complete valid frame.
  push(b) {
    switch (this.state) {
      case 0: if (b === SOF) this.state = 1; break;
      case 1: this.len = b; this.state = 2; break;
      case 2:
        this.len |= b << 8;
        if (this.len < this.min || this.len > 4096) this.reset();
        else { this.buf = []; this.state = 3; }
        break;
      case 3:
        this.buf.push(b);
        if (this.buf.length === this.len + 2) {
          const body = this.buf.slice(0, this.len);
          const crc = this.buf[this.len] | (this.buf[this.len + 1] << 8);
          this.reset();
          if (crc16(body) === crc) return Uint8Array.from(body);
        }
        break;
    }
    return null;
  }
}

// ----------------------------------------------------------------- transports
// Each transport surfaces the same hooks: onMgmt(body), onClose(). send(frameBytes)
// delivers a management frame.

class SerialTransport {
  constructor() { this.kind = "serial"; this.mgmt = new Parser(2); }
  async connect() {
    if (!("serial" in navigator)) throw new Error("Web Serial unsupported (use Chrome/Edge over https or localhost)");
    this.port = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x303a }] });
    await this.port.open({ baudRate: 115200 });
    this.writer = this.port.writable.getWriter();
    this.reader = this.port.readable.getReader();
    this._read();
  }
  async _read() {
    try {
      for (;;) {
        const { value, done } = await this.reader.read();
        if (done) { log("serial read: stream done", "err"); break; }
        for (const b of value) { const body = this.mgmt.push(b); if (body) this.onMgmt(body); }
      }
    } catch (e) { log("serial read error: " + (e && e.message || e), "err"); }
    this.onClose && this.onClose();
  }
  async send(bytes) { await this.writer.write(bytes); }
  async disconnect() {
    try { await this.reader.cancel(); } catch (_) {}
    try { this.writer.releaseLock(); } catch (_) {}
    try { await this.port.close(); } catch (_) {}
  }
}

// Web Bluetooth transport: the management service (PROTOCOL.md) over GATT. The
// Command characteristic carries request frames (write); responses arrive as
// notifications on the Response characteristic. The HID keyboard service is
// handled by the OS and is invisible here (Web Bluetooth blocklists it anyway).
const BLE_SVC  = "6f747000-0000-1000-8000-00805f9b34fb";
const BLE_CMD  = "6f747001-0000-1000-8000-00805f9b34fb";
const BLE_RESP = "6f747002-0000-1000-8000-00805f9b34fb";

class BleTransport {
  constructor() { this.kind = "ble"; this.mgmt = new Parser(2); }
  async connect() {
    if (!("bluetooth" in navigator))
      throw new Error("Web Bluetooth unsupported (use Chrome/Edge on desktop or Android, over https or localhost)");
    this.device = await navigator.bluetooth.requestDevice({
      filters: [{ namePrefix: "esp-otp" }],
      optionalServices: [BLE_SVC],            // 128-bit UUID isn't advertised; allow it explicitly
    });
    this._onDisc = () => this.onClose && this.onClose();
    this.device.addEventListener("gattserverdisconnected", this._onDisc);
    const server = await this.device.gatt.connect();
    const svc = await server.getPrimaryService(BLE_SVC);
    this.cmd = await svc.getCharacteristic(BLE_CMD);
    this.resp = await svc.getCharacteristic(BLE_RESP);
    this.resp.addEventListener("characteristicvaluechanged", (ev) => {
      const dv = ev.target.value;             // one notification = one (or part of a) frame
      for (let i = 0; i < dv.byteLength; i++) { const m = this.mgmt.push(dv.getUint8(i)); if (m) this.onMgmt(m); }
    });
    await this.resp.startNotifications();
  }
  // Write-with-response so large ADD frames use a GATT Long Write (the firmware
  // reassembles it); plain write-without-response can't exceed one MTU.
  async send(bytes) {
    if (this.cmd.writeValueWithResponse) await this.cmd.writeValueWithResponse(bytes);
    else await this.cmd.writeValue(bytes);
  }
  disconnect() {
    try { this.device.removeEventListener("gattserverdisconnected", this._onDisc); } catch (_) {}
    try { this.device.gatt.disconnect(); } catch (_) {}
    this.onClose && this.onClose();
  }
}

// --------------------------------------------------------- management client
class Device {
  constructor(transport) {
    this.t = transport;
    this.seq = 0;
    this.pending = new Map();
    transport.onMgmt = (body) => this._onMgmt(body);
  }
  _onMgmt(body) {                 // body = [type, seq, ...payload]
    const type = body[0], seq = body[1], payload = body.subarray(2);
    const p = this.pending.get(seq);
    if (p && type === (p.op | RESP_BIT)) { this.pending.delete(seq); p.resolve(payload); }
  }
  // Serialise requests: the live screen poller and the button/reboot controls
  // share one transport, and overlapping GATT writes error ("operation already
  // in progress"). Chain each request after the previous one settles.
  request(op, payload = []) {
    const run = () => this._request(op, payload);
    this._chain = (this._chain || Promise.resolve()).then(run, run);
    return this._chain;
  }
  _request(op, payload = []) {
    this.seq = (this.seq + 1) & 0xff;
    const seq = this.seq;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => { this.pending.delete(seq); reject(new Error("timeout")); }, 3000);
      this.pending.set(seq, { op, resolve: (v) => { clearTimeout(timer); resolve(v); } });
      Promise.resolve(this.t.send(encodeMgmt(op, seq, payload))).catch(reject);
    });
  }
  // Throw on a non-OK status byte; otherwise return the payload.
  static ok(p) { if (p[0] !== 0) throw new Error("device: " + (STATUS[p[0]] || p[0])); return p; }

  async ping() { Device.ok(await this.request(OP.PING)); }
  async info() {
    const p = Device.ok(await this.request(OP.INFO));
    const dv = new DataView(p.buffer, p.byteOffset);
    return {
      fw: `${p[1]}.${p[2]}.${p[3]}`, flags: p[4],
      rtc: !!(p[4] & 1), timeValid: !!(p[4] & 2),
      count: dv.getUint16(5, true), cap: dv.getUint16(7, true),
      // name_len + name appended after the fixed 9 bytes (older fw omits it).
      name: p.length >= 10
        ? new TextDecoder().decode(p.subarray(10, 10 + p[9])) : "",
    };
  }
  async setTime(epoch = Math.floor(Date.now() / 1000)) {
    const b = new Uint8Array(4); new DataView(b.buffer).setUint32(0, epoch, true);
    Device.ok(await this.request(OP.SETTIME, b));
  }
  async list() {
    const out = [];
    let start = 0, total = 0;
    do {
      const p = Device.ok(await this.request(OP.LIST, [start & 0xff]));
      const dv = new DataView(p.buffer, p.byteOffset);
      total = dv.getUint16(1, true);
      const n = p[3];
      let off = 4;
      for (let i = 0; i < n; i++) {
        const id = dv.getUint16(off, true);
        const digits = p[off + 2];
        const period = dv.getUint16(off + 3, true);
        const algo = p[off + 5];
        const llen = p[off + 6];
        off += 7;
        const label = new TextDecoder().decode(p.subarray(off, off + llen));
        off += llen;
        out.push({ id, digits, period, algo, label });
        start++;
      }
      if (n === 0) break;
    } while (start < total);
    return out;
  }
  async add({ label, secret, digits, period, algo }) {
    const lb = new TextEncoder().encode(label);
    const sb = new TextEncoder().encode(secret);
    const head = new Uint8Array(4);
    head[0] = digits; new DataView(head.buffer).setUint16(1, period, true); head[3] = algo;
    const payload = [...head, lb.length, ...lb, sb.length, ...sb];
    const p = Device.ok(await this.request(OP.ADD, payload));
    return new DataView(p.buffer, p.byteOffset).getUint16(1, true);
  }
  async remove(id) {
    const b = new Uint8Array(2); new DataView(b.buffer).setUint16(0, id, true);
    Device.ok(await this.request(OP.REMOVE, b));
  }
  // Pull the whole OLED framebuffer, paginating on `start` like host_cli's screen
  // command. Returns { width, pages, fb } — fb is column-major by page, each byte
  // an 8-pixel vertical run (bit0 = top).
  async getScreen() {
    let start = 0, total = 0, width = 0, pages = 0;
    const fb = [];
    for (;;) {
      const b = new Uint8Array(2); new DataView(b.buffer).setUint16(0, start, true);
      const p = Device.ok(await this.request(OP.SCREEN, b));
      const dv = new DataView(p.buffer, p.byteOffset);
      total = dv.getUint16(1, true); width = p[3]; pages = p[4];
      const chunk = p.subarray(5);
      if (chunk.length === 0) break;
      for (const x of chunk) fb.push(x);
      start += chunk.length;
      if (start >= total) break;
    }
    return { width, pages, fb: Uint8Array.from(fb) };
  }
  async button(gesture) { Device.ok(await this.request(OP.BUTTON, [gesture])); }
  async reboot() { Device.ok(await this.request(OP.REBOOT)); }
  async setName(name) {
    const nb = new TextEncoder().encode(name);
    Device.ok(await this.request(OP.SETNAME, [nb.length, ...nb]));
  }
  async rename(id, label) {
    const lb = new TextEncoder().encode(label);
    const b = new Uint8Array(2); new DataView(b.buffer).setUint16(0, id, true);
    Device.ok(await this.request(OP.RENAME, [...b, lb.length, ...lb]));
  }
  async move(id, index) {
    const b = new Uint8Array(3); const dv = new DataView(b.buffer);
    dv.setUint16(0, id, true); b[2] = index;
    Device.ok(await this.request(OP.MOVE, b));
  }
}

// ------------------------------------------------------------------------ UI
const $ = (id) => document.getElementById(id);
const logEl = $("log");
function log(msg, cls) {
  const t = new Date().toLocaleTimeString();
  logEl.textContent += `[${t}] ${msg}\n`;
  logEl.scrollTop = logEl.scrollHeight;
  if (cls === "err") console.warn(msg);
}

let transport = null, device = null, lastDeviceName = "";

function setConnected(on) {
  const badge = $("status");
  badge.textContent = on ? "connected" : "disconnected";
  badge.className = "badge " + (on ? "on" : "off");
  $("connect").textContent = on ? "Disconnect" : "Connect";
  for (const id of ["ping", "refresh", "settime", "setname",
                    "btn-single", "btn-double", "btn-long", "btn-reboot"])
    $(id).disabled = !on;
  $("add").querySelector('button[type=submit]').disabled = !on;
  if (on) {
    startScreen();
  } else {
    stopScreen();
    clearScreen();
    $("devname").textContent = $("fw").textContent = $("rtc").textContent = $("timevalid").textContent = $("count").textContent = "—";
    lastDeviceName = "";
    $("list").querySelector("tbody").innerHTML = "";
    $("list-empty").hidden = false; $("list-empty").textContent = "not connected";
  }
}

// ------------------------------------------------------------------ live screen
// Mirror the OLED: paint the framebuffer to the canvas, poll once a second.
let screenTimer = null, screenBusy = false, screenErrShown = false;

function drawScreen({ width, pages, fb }) {
  const h = pages * 8;
  const canvas = $("screen");
  canvas.width = width; canvas.height = h;         // internal res; CSS scales up
  const ctx = canvas.getContext("2d");
  const img = ctx.createImageData(width, h);
  for (let page = 0; page < pages; page++) {
    for (let col = 0; col < width; col++) {
      const byte = fb[page * width + col] || 0;
      for (let bit = 0; bit < 8; bit++) {
        const on = (byte >> bit) & 1;
        const idx = (((page * 8 + bit) * width) + col) * 4;
        img.data[idx] = on ? 0x39 : 0x04;          // OLED green on near-black
        img.data[idx + 1] = on ? 0xff : 0x14;
        img.data[idx + 2] = on ? 0x8c : 0x0c;
        img.data[idx + 3] = 0xff;
      }
    }
  }
  ctx.putImageData(img, 0, 0);
}

function clearScreen() {
  const canvas = $("screen");
  canvas.getContext("2d").clearRect(0, 0, canvas.width, canvas.height);
}

async function pollScreen() {
  if (!device || screenBusy) return;               // skip if a poll is still in flight
  screenBusy = true;
  try {
    drawScreen(await device.getScreen());
    screenErrShown = false;
  } catch (e) {
    if (!screenErrShown) { log("screen read failed: " + e.message, "err"); screenErrShown = true; }
  } finally {
    screenBusy = false;
  }
}

function startScreen() {
  stopScreen();
  screenErrShown = false;
  pollScreen();
  screenTimer = setInterval(pollScreen, 1000);
}

function stopScreen() {
  if (screenTimer) clearInterval(screenTimer);
  screenTimer = null;
}

function renderInfo(info) {
  $("devname").textContent = info.name || "—";
  lastDeviceName = info.name;
  $("fw").textContent = info.fw;
  $("rtc").textContent = info.rtc ? "yes" : "no";
  $("timevalid").textContent = info.timeValid ? "yes" : "no";
  $("count").textContent = `${info.count} / ${info.cap}`;
}

function renderList(rows) {
  const tb = $("list").querySelector("tbody");
  tb.innerHTML = "";
  $("list-empty").hidden = rows.length > 0;
  if (!rows.length) { $("list-empty").textContent = "(no secrets)"; return; }
  rows.forEach((r, i) => {
    const tr = document.createElement("tr");
    tr.innerHTML = `<td>${r.id}</td><td></td>` +
      `<td class="muted">${r.digits}d/${r.period}s ${ALGO[r.algo] || r.algo}</td><td></td>`;
    tr.children[1].textContent = r.label;                 // textContent: no HTML injection
    const actions = tr.children[3];
    const up = document.createElement("button"); up.textContent = "↑"; up.title = "move up";
    up.disabled = i === 0;
    up.onclick = () => doMove(r, i - 1);
    const dn = document.createElement("button"); dn.textContent = "↓"; dn.title = "move down";
    dn.disabled = i === rows.length - 1;
    dn.onclick = () => doMove(r, i + 1);
    const rn = document.createElement("button"); rn.textContent = "rename"; rn.className = "danger";
    rn.onclick = () => doRename(r);
    const rm = document.createElement("button"); rm.textContent = "remove"; rm.className = "danger";
    rm.onclick = () => doRemove(r);
    actions.append(up, " ", dn, " ", rn, " ", rm);
    tb.appendChild(tr);
  });
}

// Parse a standard otpauth://totp/<label>?secret=…&algorithm=…&digits=…&period=…
// URI into the add-form fields. Throws on anything that isn't a TOTP otpauth URL.
function parseOtpauth(raw) {
  const u = new URL(raw.trim());                 // throws on malformed input
  if (u.protocol !== "otpauth:") throw new Error("not an otpauth:// URL");
  if (u.host.toLowerCase() !== "totp") throw new Error("only otpauth totp is supported");
  const q = u.searchParams;
  const secret = (q.get("secret") || "").replace(/\s+/g, "").toUpperCase();
  if (!secret) throw new Error("URL has no secret");
  const label = decodeURIComponent(u.pathname.replace(/^\//, ""));
  const algo = { SHA1: 0, SHA256: 1, SHA512: 2 }[(q.get("algorithm") || "SHA1").toUpperCase()];
  if (algo === undefined) throw new Error("unsupported algorithm");
  return { label, secret, algo, digits: +(q.get("digits") || 6), period: +(q.get("period") || 30) };
}

async function refresh() {
  try {
    renderInfo(await device.info());
    renderList(await device.list());
  } catch (e) { log("refresh failed: " + e.message, "err"); }
}

async function doRemove(r) {
  if (!confirm(`Remove "${r.label}" (id ${r.id})?`)) return;
  try { await device.remove(r.id); log(`removed id=${r.id}`); await refresh(); }
  catch (e) { log("remove failed: " + e.message, "err"); }
}
async function doRename(r) {
  const label = prompt("New label", r.label);
  if (label == null) return;
  try { await device.rename(r.id, label); log(`renamed id=${r.id}`); await refresh(); }
  catch (e) { log("rename failed: " + e.message, "err"); }
}
async function doMove(r, index) {
  try { await device.move(r.id, index); log(`moved id=${r.id} to index ${index}`); await refresh(); }
  catch (e) { log("move failed: " + e.message, "err"); }
}

// --------------------------------------------------------------- wiring
function selectedTransport() {
  return document.querySelector('input[name=transport]:checked').value;
}

$("connect").onclick = async () => {
  if (transport) {                       // disconnect
    await transport.disconnect();
    return;                              // onClose handler resets state
  }
  try {
    if (selectedTransport() === "ble") {
      transport = new BleTransport();
    } else {
      transport = new SerialTransport();
    }
    transport.onClose = () => {
      log("disconnected");
      transport = null; device = null; setConnected(false);
    };
    await transport.connect();
    device = new Device(transport);
    setConnected(true);
    log(`connected (${transport.kind})`);
    await refresh();
  } catch (e) {
    log("connect failed: " + e.message, "err");
    $("status").className = "badge err"; $("status").textContent = "error";
    transport = null; device = null;
  }
};

$("ping").onclick = async () => {
  try { await device.ping(); log("pong"); } catch (e) { log("ping failed: " + e.message, "err"); }
};
$("refresh").onclick = refresh;
$("settime").onclick = async () => {
  try { await device.setTime(); log("time set to now"); await refresh(); }
  catch (e) { log("settime failed: " + e.message, "err"); }
};
$("setname").onclick = async () => {
  const name = prompt("New BLE device name (1..20 chars):", lastDeviceName);
  if (name === null) return;
  const trimmed = name.trim();
  const n = new TextEncoder().encode(trimmed).length;
  if (n < 1 || n > 20) { log("name must be 1..20 bytes", "err"); return; }
  try { await device.setName(trimmed); log(`device name set to ${trimmed}`); await refresh(); }
  catch (e) { log("setname failed: " + e.message, "err"); }
};

async function doButton(name) {
  try { await device.button(BUTTON[name]); log(`button ${name}`); }
  catch (e) { log(`button ${name} failed: ` + e.message, "err"); }
}
$("btn-single").onclick = () => doButton("single");
$("btn-double").onclick = () => doButton("double");
$("btn-long").onclick = () => doButton("long");
$("btn-reboot").onclick = async () => {
  if (!confirm("Reboot the device? The link will drop and reconnect.")) return;
  try { await device.reboot(); log("reboot scheduled"); }
  catch (e) { log("reboot failed: " + e.message, "err"); }
};

$("parse-url").onclick = () => {
  const f = $("add");
  try {
    const o = parseOtpauth(f.url.value);
    f.label.value = o.label.slice(0, 31);        // STORE_LABEL_MAX
    f.secret.value = o.secret;
    f.digits.value = o.digits; f.period.value = o.period; f.algo.value = o.algo;
    log(`parsed otpauth for "${o.label}"` + (o.label.length > 31 ? " (label truncated)" : ""));
  } catch (e) { log("URL parse failed: " + e.message, "err"); }
};

$("add").onsubmit = async (e) => {
  e.preventDefault();
  const f = e.target;
  try {
    const id = await device.add({
      label: f.label.value.trim(),
      secret: f.secret.value.replace(/\s+/g, "").toUpperCase(),
      digits: +f.digits.value, period: +f.period.value, algo: +f.algo.value,
    });
    log(`added id=${id}`);
    f.reset(); f.digits.value = 6; f.period.value = 30;
    await refresh();
  } catch (err) { log("add failed: " + err.message, "err"); }
};

// Initial state.
setConnected(false);
if (!("serial" in navigator))
  log("note: Web Serial not available in this browser — Web Bluetooth still works");
if (!("bluetooth" in navigator))
  log("note: Web Bluetooth not available in this browser (needs Chrome/Edge on desktop or Android, https/localhost)");
