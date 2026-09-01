// esp-otp wire protocol codec — pure Dart, no BLE, no UI.
//
// The Dart twin of tools/host_cli.py and tools/web/app.js. One framed binary
// request/response protocol (see PROTOCOL.md): SOF | LEN(2 LE) | TYPE | SEQ |
// PAYLOAD | CRC16(2 LE), CRC-16/CCITT-FALSE over TYPE+SEQ+PAYLOAD. Keep this in
// sync with host_cli.py; the unit tests pin the shared vectors.
import 'dart:typed_data';

const int sof = 0x7e;
const int respBit = 0x80;
const int maxPayload = 250;

/// Opcodes (mirror host_cli.py OP_*).
class Op {
  static const int ping = 0x01;
  static const int getInfo = 0x02;
  static const int setTime = 0x03;
  static const int list = 0x04;
  static const int add = 0x05;
  static const int remove = 0x06;
  static const int rename = 0x07;
  static const int getScreen = 0x08;
  static const int injectButton = 0x09;
  static const int reboot = 0x0a;
  static const int move = 0x0b;
  static const int setName = 0x0c;
}

/// INJECT_BUTTON gesture codes (mirror the firmware button_event_t).
class Gesture {
  static const int single = 1;
  static const int double_ = 2;
  static const int long = 3;
}

const Map<int, String> statusNames = {
  0: 'OK',
  1: 'BAD_REQUEST',
  2: 'FULL',
  3: 'NOT_FOUND',
  4: 'INVALID_ARG',
  5: 'NOT_AUTHORIZED',
  6: 'CRC_ERROR',
  7: 'UNSUPPORTED',
};

const List<String> algoNames = ['SHA1', 'SHA256', 'SHA512'];

/// A device error carrying the protocol status byte.
class DeviceError implements Exception {
  final int status;
  DeviceError(this.status);
  String get name => statusNames[status] ?? '0x${status.toRadixString(16)}';
  bool get notAuthorized => status == 5;
  @override
  String toString() => 'device error: $name';
}

/// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF).
int crc16(List<int> data) {
  int crc = 0xffff;
  for (final b in data) {
    crc ^= (b & 0xff) << 8;
    for (int i = 0; i < 8; i++) {
      crc = (crc & 0x8000) != 0
          ? ((crc << 1) ^ 0x1021) & 0xffff
          : (crc << 1) & 0xffff;
    }
  }
  return crc;
}

/// Encode a request into a full framed packet.
Uint8List encodeFrame(int type, int seq, [List<int> payload = const []]) {
  if (payload.length > maxPayload) {
    throw ArgumentError('payload too large (${payload.length} > $maxPayload)');
  }
  final body = <int>[type & 0xff, seq & 0xff, ...payload];
  final crc = crc16(body);
  final out = BytesBuilder();
  out.addByte(sof);
  out.addByte(body.length & 0xff);
  out.addByte((body.length >> 8) & 0xff);
  out.add(body);
  out.addByte(crc & 0xff);
  out.addByte((crc >> 8) & 0xff);
  return out.toBytes();
}

/// A decoded response frame.
class Frame {
  final int type;
  final int seq;
  final Uint8List payload;
  Frame(this.type, this.seq, this.payload);
}

/// Streaming frame extractor — mirrors the C / Python / JS parsers. Tolerates
/// noise between frames so it resyncs on the SOF boundary.
class FrameParser {
  static const int _sofState = 0,
      _len0 = 1,
      _len1 = 2,
      _body = 3;
  int _state = _sofState;
  int _len = 0;
  final List<int> _buf = [];

  void reset() {
    _state = _sofState;
    _len = 0;
    _buf.clear();
  }

  /// Feed one byte; return a [Frame] once a complete, CRC-valid frame arrives.
  Frame? push(int byte) {
    final b = byte & 0xff;
    switch (_state) {
      case _sofState:
        if (b == sof) _state = _len0;
        break;
      case _len0:
        _len = b;
        _state = _len1;
        break;
      case _len1:
        _len |= b << 8;
        if (_len < 2 || _len > 2 + maxPayload) {
          reset();
        } else {
          _buf.clear();
          _state = _body;
        }
        break;
      case _body:
        _buf.add(b);
        if (_buf.length == _len + 2) {
          final body = _buf.sublist(0, _len);
          final crcRecv = _buf[_len] | (_buf[_len + 1] << 8);
          reset();
          if (crc16(body) == crcRecv) {
            return Frame(
                body[0], body[1], Uint8List.fromList(body.sublist(2)));
          }
        }
        break;
    }
    return null;
  }
}
