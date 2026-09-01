// Foreground-service wiring for the background clock sync. The service keeps the
// process alive with a persistent notification; its repeat event drives
// SyncEngine.tick() so a device that appears is connected and time-synced within
// one tick, then refreshed every 10 minutes — the ble_clock_sync.py behaviour.
import 'package:flutter/foundation.dart';
import 'package:flutter_foreground_task/flutter_foreground_task.dart';

import 'known_devices.dart';
import 'sync_engine.dart';

const int kSyncServiceId = 256;
const String kSyncChannelId = 'esp_otp_sync';

/// Entry point for the foreground-service isolate. Must be a top-level function.
@pragma('vm:entry-point')
void startSyncCallback() {
  FlutterForegroundTask.setTaskHandler(_SyncTaskHandler());
}

class _SyncTaskHandler extends TaskHandler {
  SyncEngine? _engine;

  @override
  Future<void> onStart(DateTime timestamp, TaskStarter starter) async {
    final ids = await loadKnownDevices();
    _engine = SyncEngine(
      deviceIds: ids,
      log: (m) => debugPrint('[sync] $m'),
    );
    // Arm autoConnect; the connect event (not a poll) triggers the first sync.
    await _engine!.start();
    _updateNotification();
  }

  @override
  void onRepeatEvent(DateTime timestamp) {
    // The only periodic work: re-push time to already-connected devices.
    // Discovery/reconnect are event-driven via autoConnect, not here.
    _engine?.refreshAll().then((_) => _updateNotification());
  }

  @override
  Future<void> onDestroy(DateTime timestamp) async {
    await _engine?.stop();
    _engine = null;
  }

  void _updateNotification() {
    final n = _engine?.connectedCount ?? 0;
    FlutterForegroundTask.updateService(
      notificationTitle: 'esp-otp clock sync',
      notificationText: n == 0
          ? 'Waiting for a device to connect…'
          : 'Synced $n device${n == 1 ? '' : 's'}',
    );
  }

  @override
  void onReceiveData(Object data) {}
  @override
  void onNotificationButtonPressed(String id) {}
  @override
  void onNotificationPressed() {
    FlutterForegroundTask.launchApp();
  }
}

/// Configure the foreground task once (call before starting the service).
void initForegroundTask() {
  FlutterForegroundTask.init(
    androidNotificationOptions: AndroidNotificationOptions(
      channelId: kSyncChannelId,
      channelName: 'esp-otp clock sync',
      channelDescription: 'Keeps the device clock synced over BLE',
      channelImportance: NotificationChannelImportance.LOW,
      priority: NotificationPriority.LOW,
    ),
    iosNotificationOptions: const IOSNotificationOptions(),
    foregroundTaskOptions: ForegroundTaskOptions(
      // Event-driven discovery (autoConnect) means the only periodic work is
      // the ~10-min clock refresh; the repeat event drives that under doze.
      eventAction: ForegroundTaskEventAction.repeat(600000), // 10 min
      autoRunOnBoot: false,
      autoRunOnMyPackageReplaced: false,
      allowWakeLock: true,
      allowWifiLock: false,
    ),
  );
}

Future<void> startSyncService() async {
  if (await FlutterForegroundTask.isRunningService) return;
  await FlutterForegroundTask.startService(
    serviceId: kSyncServiceId,
    notificationTitle: 'esp-otp clock sync',
    notificationText: 'Starting…',
    callback: startSyncCallback,
  );
}

Future<void> stopSyncService() async {
  if (await FlutterForegroundTask.isRunningService) {
    await FlutterForegroundTask.stopService();
  }
}

Future<bool> isSyncRunning() => FlutterForegroundTask.isRunningService;
