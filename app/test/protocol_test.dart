// Protocol codec tests — the Dart side of host_cli.py's --selftest. Pins the
// shared vectors so the app framing/CRC can't drift from the firmware.
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:esp_otp/proto/protocol.dart';
import 'package:esp_otp/proto/otpauth.dart';

void main() {
  test('CRC-16/CCITT-FALSE known vector', () {
    expect(crc16('123456789'.codeUnits), 0x29B1);
  });

  test('encode/parse round-trip (ADD)', () {
    final f = encodeFrame(Op.add, 0x11, [0xDE, 0xAD, 0xBE, 0xEF, 0x42]);
    final p = FrameParser();
    Frame? out;
    for (final b in f) {
      out = p.push(b) ?? out;
    }
    expect(out, isNotNull);
    expect(out!.type, Op.add);
    expect(out.seq, 0x11);
    expect(out.payload, Uint8List.fromList([0xDE, 0xAD, 0xBE, 0xEF, 0x42]));
  });

  test('empty-payload frame (PING)', () {
    final f = encodeFrame(Op.ping, 1);
    final p = FrameParser();
    Frame? out;
    for (final b in f) {
      out = p.push(b) ?? out;
    }
    expect(out!.type, Op.ping);
    expect(out.seq, 1);
    expect(out.payload.isEmpty, true);
  });

  test('parser resyncs after leading noise', () {
    final f = encodeFrame(Op.ping, 7);
    final p = FrameParser();
    Frame? out;
    // Benign leading bytes (no false 0x7E) that the parser skips before the SOF.
    for (final b in [0x00, 0xFF, 0x11, ...f]) {
      out = p.push(b) ?? out;
    }
    expect(out, isNotNull);
    expect(out!.seq, 7);
  });

  test('parser rejects a corrupted CRC', () {
    final f = Uint8List.fromList(encodeFrame(Op.ping, 1));
    f[f.length - 1] ^= 0xFF; // flip a CRC byte
    final p = FrameParser();
    Frame? out;
    for (final b in f) {
      out = p.push(b) ?? out;
    }
    expect(out, isNull);
  });

  test('parseOtpauth: label from path, params override defaults', () {
    final s = parseOtpauth(
        'otpauth://totp/GitHub:me%40x.com?secret=JBSWY3DPEHPK3PXP'
        '&issuer=GitHub&algorithm=SHA256&digits=8&period=60');
    expect(s.label, 'GitHub:me@x.com');
    expect(s.secret, 'JBSWY3DPEHPK3PXP');
    expect(s.digits, 8);
    expect(s.period, 60);
    expect(s.algo, 1);
  });

  test('parseOtpauth: defaults, label falls back to issuer', () {
    final s = parseOtpauth('otpauth://totp/?secret=ABCD&issuer=ACME');
    expect(s.label, 'ACME');
    expect(s.digits, 6);
    expect(s.period, 30);
    expect(s.algo, 0);
  });

  test('parseOtpauth rejects non-totp / missing secret', () {
    for (final bad in [
      'otpauth://hotp/x?secret=A',
      'otpauth://totp/x',
      'https://x',
    ]) {
      expect(() => parseOtpauth(bad), throwsA(isA<FormatException>()));
    }
  });

  test('checkBase32 accepts alphabet + separators, rejects junk', () {
    for (final ok in ['JBSWY3DPEHPK3PXP', 'jbsw y3dp ee======', 'GEZD-GNBV']) {
      checkBase32(ok); // no throw
    }
    for (final bad in ['', '   ', 'ABC0', 'hello!', '18990']) {
      expect(() => checkBase32(bad), throwsA(isA<FormatException>()));
    }
  });
}
