// flutter_blue_plus glue: scan for esp-otp by its advertised management service
// UUID (rename-proof, like the CLI), open a connection, wire the Command/
// Response characteristics into a [Transport], and expose an [OtpDevice].
import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/foundation.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import 'otp_device.dart';
import 'transport.dart';

// Management service + characteristics (see PROTOCOL.md "BLE GATT layout").
final Guid mgmtService = Guid('6f747000-0000-1000-8000-00805f9b34fb');
final Guid cmdChar = Guid('6f747001-0000-1000-8000-00805f9b34fb');
final Guid rspChar = Guid('6f747002-0000-1000-8000-00805f9b34fb');

/// A live GATT transport over the Command/Response characteristics.
class BleTransport implements Transport {
  final BluetoothCharacteristic _cmd;
  final BluetoothCharacteristic _rsp;
  BleTransport(this._cmd, this._rsp);

  @override
  Stream<List<int>> get incoming => _rsp.onValueReceived;

  @override
  Future<void> send(Uint8List frame) async {
    // Write-with-response so a large ADD frame uses a GATT long write and we
    // know the device accepted it.
    await _cmd.write(frame, withoutResponse: false);
  }

  @override
  Future<void> dispose() async {}
}

/// One connected esp-otp: the BluetoothDevice plus its ready [OtpDevice].
class EspOtpConnection {
  final BluetoothDevice device;
  final OtpDevice otp;
  final BleTransport _transport;

  EspOtpConnection._(this.device, this.otp, this._transport);

  String get id => device.remoteId.str;
  String get name => device.platformName.isNotEmpty
      ? device.platformName
      : (device.advName.isNotEmpty ? device.advName : 'esp-otp');

  /// Connect, discover the management service, subscribe to notifications and
  /// return a ready connection. Caller owns [close].
  static Future<EspOtpConnection> open(BluetoothDevice device,
      {Duration timeout = const Duration(seconds: 20)}) async {
    if (!device.isConnected) {
      await device.connect(timeout: timeout, mtu: null);
    }
    // Bigger MTU lets LIST/ADD/GET_SCREEN fit far fewer round-trips (a small MTU
    // is the main cause of slow, contended reads). Request the max; the stack
    // caps it. Best-effort — log what we actually got.
    try {
      final mtu = await device.requestMtu(512);
      debugPrint('esp-otp: negotiated MTU $mtu');
    } catch (e) {
      debugPrint('esp-otp: requestMtu failed ($e) — using default MTU');
    }

    final services = await device.discoverServices();
    final svc = services.firstWhere(
      (s) => s.uuid == mgmtService,
      orElse: () => throw StateError('esp-otp management service not found'),
    );
    final cmd = svc.characteristics.firstWhere((c) => c.uuid == cmdChar);
    final rsp = svc.characteristics.firstWhere((c) => c.uuid == rspChar);
    await rsp.setNotifyValue(true);

    final transport = BleTransport(cmd, rsp);
    final otp = OtpDevice(transport);
    return EspOtpConnection._(device, otp, transport);
  }

  Future<void> close({bool disconnect = true}) async {
    await otp.dispose();
    await _transport.dispose();
    if (disconnect) {
      try {
        await device.disconnect();
      } catch (_) {}
    }
  }
}

/// Scan for esp-otp devices, matched by the advertised management service UUID.
/// Emits the running de-duplicated result set until [stop] or timeout.
Stream<List<ScanResult>> scanEspOtp(
    {Duration timeout = const Duration(seconds: 15)}) async* {
  await FlutterBluePlus.startScan(
    withServices: [mgmtService],
    timeout: timeout,
  );
  yield* FlutterBluePlus.scanResults;
}

Future<void> stopScan() => FlutterBluePlus.stopScan();
