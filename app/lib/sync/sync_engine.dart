// Clock-sync engine — the Dart port of tools/ble_clock_sync.py, event-driven.
//
// Instead of scan-polling, it arms an OS-level autoConnect for each remembered
// esp-otp: the Android BLE controller waits for the device to advertise and
// connects on its own (low power, survives sleep). We react to the connection
// event — SET_TIME immediately on connect — and re-arm is automatic on drop, so
// discovery and reconnect are both interrupt-driven. A single [refreshAll] call
// (driven by the foreground service's repeat event) re-pushes time every ~10
// min. SET_TIME needs an authenticated bond — pair once.
import 'dart:async';

import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import '../ble/ble.dart';
import '../proto/protocol.dart';

typedef LogFn = void Function(String msg);

class _Conn {
  final EspOtpConnection conn;
  DateTime lastSync = DateTime.fromMillisecondsSinceEpoch(0);
  _Conn(this.conn);
}

class SyncEngine {
  final List<String> deviceIds; // remembered esp-otp ids to autoConnect-arm
  final LogFn log;

  // Live OtpDevice connections, keyed by remoteId.
  final Map<String, _Conn> _conns = {};
  // connectionState listeners, kept for the life of the engine so autoConnect
  // reconnect events keep arriving after a drop.
  final Map<String, StreamSubscription<BluetoothConnectionState>> _subs = {};
  final Set<String> _busy = {}; // ids whose connect setup is in flight

  SyncEngine({required this.deviceIds, LogFn? log}) : log = log ?? _noop;

  static void _noop(String _) {}

  int get connectedCount => _conns.length;

  /// Arm an OS autoConnect for every remembered device. Returns immediately;
  /// the controller connects whenever a device advertises.
  Future<void> start() async {
    for (final id in deviceIds) {
      await _arm(id);
    }
    log('armed autoConnect for ${deviceIds.length} device(s)');
  }

  Future<void> _arm(String id) async {
    if (_subs.containsKey(id)) return;
    final d = BluetoothDevice.fromId(id);
    _subs[id] = d.connectionState.listen((s) => _onState(d, s));
    try {
      // autoConnect requires mtu:null; we raise the MTU after the link is up.
      await d.connect(autoConnect: true, mtu: null);
    } catch (e) {
      log('arm $id failed: $e');
    }
  }

  void _onState(BluetoothDevice d, BluetoothConnectionState s) {
    final id = d.remoteId.str;
    if (s == BluetoothConnectionState.connected) {
      _onConnected(d);
    } else if (s == BluetoothConnectionState.disconnected) {
      _onDisconnected(id);
    }
  }

  Future<void> _onConnected(BluetoothDevice d) async {
    final id = d.remoteId.str;
    if (_conns.containsKey(id) || _busy.contains(id)) return;
    _busy.add(id);
    try {
      // The link is already up (autoConnect); open() just raises the MTU,
      // discovers the management service and subscribes to notifications.
      final conn = await EspOtpConnection.open(d);
      _conns[id] = _Conn(conn);
      // Auto-sync only ever targets already-bonded devices; open the command
      // gate only when the link is actually bonded (secure). Query the OS
      // bonded-devices list (the bondState stream's initial value is unreliable
      // on a fresh process). If not bonded, skip — the firmware would reject
      // SET_TIME on an unauthenticated link anyway.
      final osBonded = await FlutterBluePlus.bondedDevices;
      final bonded = osBonded.any((b) => b.remoteId == d.remoteId);
      conn.otp.secure = bonded;
      log('connected to ${conn.name}${bonded ? '' : ' (not bonded — skip sync)'}');
      if (bonded) await _pushTime(id); // immediate sync on connect
    } catch (e) {
      // Leave autoConnect armed — never disconnect here, or we'd cancel it.
      log('setup $id failed: $e');
    } finally {
      _busy.remove(id);
    }
  }

  Future<void> _onDisconnected(String id) async {
    final c = _conns.remove(id);
    if (c == null) return;
    // Tear down GATT resources but keep autoConnect armed (don't disconnect the
    // device) so the stack reconnects on the next advertisement.
    await c.conn.close(disconnect: false);
    log('${c.conn.name} disconnected (re-arm pending)');
  }

  /// Re-push host time to every connected device. Call on a ~10-min cadence.
  Future<void> refreshAll() async {
    for (final id in _conns.keys.toList()) {
      await _pushTime(id);
    }
  }

  Future<void> _pushTime(String id) async {
    final c = _conns[id];
    if (c == null) return;
    try {
      await c.conn.otp.setTime();
      c.lastSync = DateTime.now();
      log('${c.conn.name}: clock synced');
    } on DeviceError catch (e) {
      log('${c.conn.name}: ${e.notAuthorized ? "not paired — pair once" : e.name}');
    } catch (e) {
      log('${c.conn.name}: sync failed ($e)');
    }
  }

  /// Tear everything down: cancel autoConnect (via disconnect) and listeners.
  Future<void> stop() async {
    for (final sub in _subs.values) {
      await sub.cancel();
    }
    _subs.clear();
    for (final c in _conns.values) {
      await c.conn.close(disconnect: true); // disconnect cancels autoConnect
    }
    _conns.clear();
    // Cancel any still-armed autoConnect for devices that never connected.
    for (final id in deviceIds) {
      try {
        await BluetoothDevice.fromId(id).disconnect();
      } catch (_) {}
    }
  }
}
