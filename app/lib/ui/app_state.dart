// Foreground app state: permissions, scanning, the active connection and the
// device operations the UI drives. A ChangeNotifier so widgets rebuild on
// change (no extra state-management dependency).
import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:flutter_foreground_task/flutter_foreground_task.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../ble/ble.dart';
import '../ble/otp_device.dart';
import '../proto/models.dart';
import '../proto/otpauth.dart';
import '../sync/ble_wake.dart';
import '../sync/known_devices.dart';
import '../sync/sync_task.dart';

enum ConnState { idle, connecting, connected }

class AppState extends ChangeNotifier {
  static const _kLastDevice = 'last_device_id';

  final List<ScanResult> scanResults = [];
  bool scanning = false;
  BluetoothAdapterState adapterState = BluetoothAdapterState.unknown;

  ConnState conn = ConnState.idle;
  EspOtpConnection? _active;
  DeviceInfo? info;
  List<SecretEntry> secrets = [];
  String? lastError;
  bool syncServiceRunning = false;

  EspOtpConnection? get active => _active;
  OtpDevice? get otp => _active?.otp;

  void clearError() {
    lastError = null;
    notifyListeners();
  }

  StreamSubscription? _scanSub;
  StreamSubscription? _adapterSub;
  StreamSubscription<BluetoothConnectionState>? _connSub;
  StreamSubscription<BluetoothBondState>? _bondSub;

  Future<void> init() async {
    _adapterSub = FlutterBluePlus.adapterState.listen((s) {
      adapterState = s;
      notifyListeners();
    });
    _scanSub = FlutterBluePlus.scanResults.listen((rs) {
      scanResults
        ..clear()
        ..addAll(rs);
      notifyListeners();
    });
    FlutterBluePlus.isScanning.listen((s) {
      scanning = s;
      notifyListeners();
    });
    syncServiceRunning = await isSyncRunning();
    notifyListeners();
  }

  Future<bool> ensurePermissions() async {
    final req = await [
      Permission.bluetoothScan,
      Permission.bluetoothConnect,
      Permission.locationWhenInUse, // needed for scan on Android <= 30
      Permission.notification,
    ].request();
    final ok = (req[Permission.bluetoothScan]?.isGranted ?? false) &&
        (req[Permission.bluetoothConnect]?.isGranted ?? false);
    if (!ok) lastError = 'Bluetooth permissions denied';
    return ok;
  }

  // ------------------------------------------------------------------ scanning
  Future<void> startScan() async {
    if (!await ensurePermissions()) {
      notifyListeners();
      return;
    }
    lastError = null;
    scanResults.clear();
    notifyListeners();
    try {
      await FlutterBluePlus.startScan(
          withServices: [mgmtService], timeout: const Duration(seconds: 15));
    } catch (e) {
      lastError = 'scan failed: $e';
      notifyListeners();
    }
  }

  Future<void> stopScanning() async {
    try {
      await stopScan();
    } catch (_) {}
  }

  // ---------------------------------------------------------------- connection
  Future<void> connect(BluetoothDevice device) async {
    conn = ConnState.connecting;
    lastError = null;
    notifyListeners();
    await stopScanning();
    try {
      final c = await EspOtpConnection.open(device);
      _active = c;
      _connSub = device.connectionState.listen((s) {
        if (s == BluetoothConnectionState.disconnected) _onDisconnected();
      });
      // Gate all commands on the OS bond state: only a bonded (encrypted) link
      // is secure, so nothing is sent until pairing is fully established.
      // Seed from the OS bonded-devices list rather than the bondState stream:
      // on a fresh app process the stream has no cached value to emit for an
      // already-bonded device, so the listener alone would never flip secure.
      // The listener still catches a bond that completes AFTER connecting
      // (first-time pairing done from the phone's Bluetooth settings).
      _bondSub = device.bondState.listen((bs) {
        if (bs == BluetoothBondState.bonded && !(c.otp.secure)) {
          c.otp.secure = true;
          refresh();
        }
      });
      conn = ConnState.connected;
      await _persistLast(device.remoteId.str);
      notifyListeners();
      await _seedSecureFromBond(device, c);
    } catch (e) {
      conn = ConnState.idle;
      lastError = 'connect failed: $e';
      _active = null;
      notifyListeners();
    }
  }

  // Query the OS for its currently-bonded devices and open the command gate if
  // this device is among them. Definitive and immediate, unlike the bondState
  // stream whose initial value is unreliable on a fresh process.
  Future<void> _seedSecureFromBond(
      BluetoothDevice device, EspOtpConnection c) async {
    try {
      final bonded = await FlutterBluePlus.bondedDevices;
      if (bonded.any((d) => d.remoteId == device.remoteId)) {
        if (!c.otp.secure) {
          c.otp.secure = true;
          await refresh();
        }
      }
    } catch (e) {
      lastError = 'bond check failed: $e';
      notifyListeners();
    }
  }

  void _onDisconnected() {
    _connSub?.cancel();
    _bondSub?.cancel();
    _active = null;
    info = null;
    secrets = [];
    conn = ConnState.idle;
    notifyListeners();
  }

  Future<void> disconnect() async {
    await _connSub?.cancel();
    await _bondSub?.cancel();
    await _active?.close();
    _onDisconnected();
  }

  Future<void> _persistLast(String id) async {
    final p = await SharedPreferences.getInstance();
    await p.setString(_kLastDevice, id);
    // Remember it so the background service can autoConnect-arm it later.
    await addKnownDevice(id);
  }

  // ---------------------------------------------------------- device operations
  Future<T?> _guard<T>(Future<T> Function(OtpDevice d) op) async {
    final d = otp;
    if (d == null) return null;
    try {
      lastError = null;
      final r = await op(d);
      return r;
    } catch (e) {
      lastError = '$e';
      notifyListeners();
      return null;
    }
  }

  Future<void> refresh() async {
    final i = await _guard((d) => d.info());
    if (i != null) info = i;
    final s = await _guard((d) => d.list());
    if (s != null) secrets = s;
    notifyListeners();
  }

  Future<bool> syncTime() async {
    final r = await _guard((d) async {
      await d.setTime();
      return true;
    });
    if (r == true) await refresh();
    return r == true;
  }

  Future<bool> addSecret(OtpSpec spec) async {
    final r = await _guard((d) => d.add(spec));
    if (r != null) await refresh();
    return r != null;
  }

  Future<void> removeSecret(int id) async {
    await _guard((d) => d.remove(id));
    await refresh();
  }

  Future<void> renameSecret(int id, String label) async {
    await _guard((d) => d.rename(id, label));
    await refresh();
  }

  Future<void> moveSecret(int id, int index) async {
    await _guard((d) => d.move(id, index));
    await refresh();
  }

  Future<void> setName(String name) async {
    await _guard((d) => d.setName(name));
    await refresh();
  }

  Future<void> reboot() async => _guard((d) => d.reboot());

  Future<void> injectButton(int gesture) async =>
      _guard((d) => d.button(gesture));

  // ---------------------------------------------------- background sync service
  Future<void> setBackgroundSync(bool on) async {
    if (on) {
      await ensurePermissions();
      await FlutterForegroundTask.requestIgnoreBatteryOptimization();
      await startSyncService();
      // Mark this process as the active syncer BEFORE arming the wake scan, so
      // the wake receiver defers to the live foreground SyncEngine instead of
      // arming a duplicate autoConnect (which would leak GATT clients).
      await setAppSyncing(true);
      // Arm the OS-level wake scan so sync still resumes after full process
      // death (native offloaded scan → SyncWakeService → bleWakeEntrypoint).
      await registerBleWake();
    } else {
      await setAppSyncing(false);
      await unregisterBleWake();
      await stopSyncService();
    }
    syncServiceRunning = await isSyncRunning();
    notifyListeners();
  }

  @override
  void dispose() {
    _scanSub?.cancel();
    _adapterSub?.cancel();
    _connSub?.cancel();
    _bondSub?.cancel();
    super.dispose();
  }
}
