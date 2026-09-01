// Management client over a [Transport]. Serialises requests (one in flight),
// tags each with a rolling SEQ, and matches the response frame — mirrors
// host_cli.py's Device.request() and app.js's Device.request().
import 'dart:async';
import 'dart:typed_data';

import '../proto/protocol.dart';
import '../proto/models.dart';
import '../proto/otpauth.dart';
import 'transport.dart';

/// Request priorities. Higher runs first; ties break FIFO. The OLED poller uses
/// [screen] so a user command (default) preempts it between paginated pages.
class Prio {
  static const int screen = 0;
  static const int normal = 10;
}

class _Job {
  final int op;
  final List<int> payload;
  final int prio;
  final int order; // insertion order, FIFO tie-break
  final Completer<Uint8List> completer = Completer<Uint8List>();
  _Job(this.op, this.payload, this.prio, this.order);
}

class OtpDevice {
  final Transport _t;
  final FrameParser _parser = FrameParser();
  late final StreamSubscription<List<int>> _sub;

  int _seq = 0;
  ({int seq, int respType, Completer<Uint8List> completer})? _pending;
  final Duration timeout;

  // Priority queue of pending jobs, pumped one at a time (the transport allows
  // only one in-flight GATT write). Each atomic request() enqueues a job; the
  // pump always dispatches the highest-priority waiter next, so a high-priority
  // user command jumps ahead of the low-priority screen poller's next page.
  final List<_Job> _q = [];
  int _order = 0;
  bool _pumping = false;

  // While paused, the scheduler holds all requests — used during OS bonding so
  // no app ATT traffic (notably the 1 Hz screen poll) competes with the SMP
  // encryption handshake, which on the ESP32-C3 causes it to time out. Callers
  // that poll (ScreenView) should also skip enqueuing while paused so requests
  // don't pile up. Clearing it resumes the pump.
  bool _paused = false;
  bool get paused => _paused;
  set paused(bool v) {
    _paused = v;
    if (!v) _pump();
  }

  // Gate: no request is sent to the device until the BLE link is fully secure
  // (bonded + encrypted). The firmware already rejects every protected command
  // on an unauthenticated link, but sending anything before pairing completes
  // also risks flooding the SMP handshake and timing it out. Driven from the
  // OS bond state (see AppState). Set true → the pump drains anything queued.
  bool _secure = false;
  bool get secure => _secure;
  set secure(bool v) {
    if (_secure == v) return;
    _secure = v;
    if (v) _pump();
  }

  OtpDevice(this._t, {this.timeout = const Duration(seconds: 5)}) {
    _sub = _t.incoming.listen(_onBytes);
  }

  void _onBytes(List<int> data) {
    for (final b in data) {
      final frame = _parser.push(b);
      if (frame == null) continue;
      final p = _pending;
      if (p != null &&
          frame.type == p.respType &&
          frame.seq == p.seq &&
          !p.completer.isCompleted) {
        p.completer.complete(frame.payload);
      }
    }
  }

  /// Enqueue a request at [prio] (default [Prio.normal]); the pump runs the
  /// highest-priority waiter next. Overlapping GATT writes never collide because
  /// only one job is in flight at a time.
  Future<Uint8List> request(int op,
      [List<int> payload = const [], int prio = Prio.normal]) {
    if (!_secure) {
      return Future.error(StateError(
          'refusing to send opcode 0x${op.toRadixString(16)}: '
          'BLE link not secure (device not bonded yet)'));
    }
    final job = _Job(op, payload, prio, _order++);
    _q.add(job);
    _pump();
    return job.completer.future;
  }

  Future<void> _pump() async {
    if (_pumping || _paused) return;
    _pumping = true;
    try {
      while (_q.isNotEmpty) {
        // Highest prio first; FIFO within the same prio.
        _q.sort((a, b) =>
            a.prio != b.prio ? b.prio - a.prio : a.order - b.order);
        final job = _q.removeAt(0);
        try {
          job.completer.complete(await _request(job.op, job.payload));
        } catch (e, st) {
          if (!job.completer.isCompleted) job.completer.completeError(e, st);
        }
      }
    } finally {
      _pumping = false;
    }
  }

  Future<Uint8List> _request(int op, List<int> payload) async {
    _seq = (_seq + 1) & 0xff;
    final seq = _seq;
    final c = Completer<Uint8List>();
    _pending = (seq: seq, respType: op | respBit, completer: c);
    try {
      await _t.send(encodeFrame(op, seq, payload));
      return await c.future.timeout(timeout,
          onTimeout: () => throw TimeoutException(
              'no response to opcode 0x${op.toRadixString(16)}'));
    } finally {
      _pending = null;
    }
  }

  /// Throw [DeviceError] on a non-OK status byte; otherwise return the payload.
  static Uint8List _ok(Uint8List p) {
    if (p.isEmpty) throw DeviceError(0xff);
    if (p[0] != 0) throw DeviceError(p[0]);
    return p;
  }

  // ---------------------------------------------------------------- commands

  Future<void> ping() async => _ok(await request(Op.ping));

  Future<DeviceInfo> info() async =>
      DeviceInfo.parse(_ok(await request(Op.getInfo)));

  Future<void> setTime([int? epoch]) async {
    final now = epoch ?? DateTime.now().millisecondsSinceEpoch ~/ 1000;
    final b = ByteData(4)..setUint32(0, now, Endian.little);
    _ok(await request(Op.setTime, b.buffer.asUint8List()));
  }

  Future<List<SecretEntry>> list() async {
    final out = <SecretEntry>[];
    int start = 0, total = 0;
    do {
      final p = _ok(await request(Op.list, [start & 0xff]));
      final bd = ByteData.sublistView(p);
      total = bd.getUint16(1, Endian.little);
      final n = p[3];
      int off = 4;
      for (int i = 0; i < n; i++) {
        final id = bd.getUint16(off, Endian.little);
        final digits = p[off + 2];
        final period = bd.getUint16(off + 3, Endian.little);
        final algo = p[off + 5];
        final llen = p[off + 6];
        off += 7;
        final label = String.fromCharCodes(p.sublist(off, off + llen));
        off += llen;
        out.add(SecretEntry(
            id: id, digits: digits, period: period, algo: algo, label: label));
        start++;
      }
      if (n == 0) break;
    } while (start < total);
    return out;
  }

  Future<int> add(OtpSpec s) async {
    checkBase32(s.secret);
    final label = Uint8List.fromList(s.label.codeUnits);
    final secret = Uint8List.fromList(s.secret.codeUnits);
    final head = ByteData(4)
      ..setUint8(0, s.digits)
      ..setUint16(1, s.period, Endian.little)
      ..setUint8(3, s.algo);
    final payload = <int>[
      ...head.buffer.asUint8List(),
      label.length, ...label,
      secret.length, ...secret,
    ];
    final p = _ok(await request(Op.add, payload));
    return ByteData.sublistView(p).getUint16(1, Endian.little);
  }

  Future<void> remove(int id) async {
    final b = ByteData(2)..setUint16(0, id, Endian.little);
    _ok(await request(Op.remove, b.buffer.asUint8List()));
  }

  Future<void> rename(int id, String label) async {
    final lb = Uint8List.fromList(label.codeUnits);
    final b = ByteData(2)..setUint16(0, id, Endian.little);
    _ok(await request(Op.rename, [...b.buffer.asUint8List(), lb.length, ...lb]));
  }

  Future<void> move(int id, int index) async {
    final b = ByteData(3)
      ..setUint16(0, id, Endian.little)
      ..setUint8(2, index);
    _ok(await request(Op.move, b.buffer.asUint8List()));
  }

  Future<void> setName(String name) async {
    final nb = Uint8List.fromList(name.codeUnits);
    _ok(await request(Op.setName, [nb.length, ...nb]));
  }

  Future<void> button(int gesture) async =>
      _ok(await request(Op.injectButton, [gesture]));

  Future<void> reboot() async => _ok(await request(Op.reboot));

  /// Pull the whole OLED framebuffer (paginated). Returns width, pages and the
  /// column-major bytes (each byte an 8-pixel vertical run, bit0 = top).
  Future<({int width, int pages, Uint8List fb})> getScreen() async {
    int start = 0, total = 0, width = 0, pages = 0;
    final fb = <int>[];
    for (;;) {
      final b = ByteData(2)..setUint16(0, start, Endian.little);
      final p = _ok(
          await request(Op.getScreen, b.buffer.asUint8List(), Prio.screen));
      final bd = ByteData.sublistView(p);
      total = bd.getUint16(1, Endian.little);
      width = p[3];
      pages = p[4];
      final chunk = p.sublist(5);
      if (chunk.isEmpty) break;
      fb.addAll(chunk);
      start += chunk.length;
      if (start >= total) break;
    }
    return (width: width, pages: pages, fb: Uint8List.fromList(fb));
  }

  Future<void> dispose() async {
    await _sub.cancel();
  }
}
