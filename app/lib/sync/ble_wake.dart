// Process-death survival: a native hardware-offloaded BLE scan (registered with
// the OS via a PendingIntent) re-launches the app when an esp-otp advertises,
// even if the process was killed. The native SyncWakeService then runs
// [bleWakeEntrypoint] in a background isolate to sync the clock on connect.
//
// Native side: android_overlay/.../BleWake.kt, BleScanReceiver.kt,
// SyncWakeService.kt, BootReceiver.kt.
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

import 'known_devices.dart';
import 'sync_engine.dart';

const MethodChannel _wake = MethodChannel('esp_otp/ble_wake');

/// Register the OS-level scan so the app is woken on a matching advertisement.
Future<void> registerBleWake() async {
  try {
    await _wake.invokeMethod('register');
  } catch (e) {
    debugPrint('registerBleWake failed: $e');
  }
}

/// Cancel the OS-level wake scan.
Future<void> unregisterBleWake() async {
  try {
    await _wake.invokeMethod('unregister');
  } catch (e) {
    debugPrint('unregisterBleWake failed: $e');
  }
}

/// Tell the native side whether this (live) app process is already running its
/// foreground sync. When true, the wake receiver skips starting a second
/// SyncEngine for the same device — avoids arming duplicate autoConnects and
/// exhausting Android's concurrent-GATT-client limit. Resets to false naturally
/// on process death (it's an in-memory native flag).
Future<void> setAppSyncing(bool on) async {
  try {
    await _wake.invokeMethod('setAppSyncing', on);
  } catch (e) {
    debugPrint('setAppSyncing failed: $e');
  }
}

/// Background isolate entry point invoked by the native SyncWakeService when the
/// OS wakes us. Runs the sync engine briefly: autoConnect the remembered
/// devices, push SET_TIME on connect, then tear down and signal completion so
/// the service can stop. Must be a top-level function kept by tree-shaking.
@pragma('vm:entry-point')
void bleWakeEntrypoint() async {
  WidgetsFlutterBinding.ensureInitialized();
  const bg = MethodChannel('esp_otp/ble_wake_bg');
  SyncEngine? engine;
  try {
    final ids = await loadKnownDevices();
    engine = SyncEngine(deviceIds: ids, log: (m) => debugPrint('[wake] $m'));
    await engine.start();
    // Give the OS autoConnect time to link and the on-connect SET_TIME to run.
    await Future.delayed(const Duration(seconds: 60));
  } catch (e) {
    debugPrint('bleWake sync failed: $e');
  } finally {
    try {
      await engine?.stop();
    } catch (_) {}
    // Tell the native service we're done so it can stop the foreground service.
    try {
      await bg.invokeMethod('done');
    } catch (_) {}
  }
}
