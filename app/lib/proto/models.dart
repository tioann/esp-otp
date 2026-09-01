// Value types decoded from GET_INFO and LIST (see PROTOCOL.md).
import 'dart:typed_data';

/// Decoded GET_INFO response (payload after the status byte).
class DeviceInfo {
  final int fwMajor, fwMinor, fwPatch;
  final int flags;
  final int count, capacity;
  final String name;

  DeviceInfo({
    required this.fwMajor,
    required this.fwMinor,
    required this.fwPatch,
    required this.flags,
    required this.count,
    required this.capacity,
    required this.name,
  });

  bool get rtcPresent => (flags & 1) != 0;
  bool get timeValid => (flags & 2) != 0;
  String get fw => '$fwMajor.$fwMinor.$fwPatch';

  /// Parse a GET_INFO payload (the full response payload, status byte included).
  factory DeviceInfo.parse(Uint8List p) {
    final bd = ByteData.sublistView(p);
    final count = bd.getUint16(5, Endian.little);
    final cap = bd.getUint16(7, Endian.little);
    // name_len + name are appended after the fixed 9 bytes (older fw omits them).
    String name = '';
    if (p.length >= 10) {
      final nlen = p[9];
      final end = (10 + nlen).clamp(10, p.length);
      name = String.fromCharCodes(p.sublist(10, end));
    }
    return DeviceInfo(
      fwMajor: p[1],
      fwMinor: p[2],
      fwPatch: p[3],
      flags: p[4],
      count: count,
      capacity: cap,
      name: name,
    );
  }
}

/// One stored TOTP record from LIST (never carries the secret).
class SecretEntry {
  final int id;
  final int digits;
  final int period;
  final int algo;
  final String label;

  SecretEntry({
    required this.id,
    required this.digits,
    required this.period,
    required this.algo,
    required this.label,
  });
}
