package com.espotp.esp_otp

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.Log
import io.flutter.FlutterInjector
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.embedding.engine.dart.DartExecutor
import io.flutter.plugin.common.MethodChannel
import io.flutter.plugins.GeneratedPluginRegistrant

/// A short-lived foreground service that boots a background Flutter engine and
/// runs the `bleWakeEntrypoint` Dart function (see ble_wake.dart) to sync the
/// clock when the OS woke us on a BLE advertisement. It stops when Dart reports
/// done, or after a safety timeout.
class SyncWakeService : Service() {
    private var engine: FlutterEngine? = null
    private val handler = Handler(Looper.getMainLooper())
    private val timeout = Runnable {
        Log.w(TAG, "wake sync timed out — stopping")
        stopSelf()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startInForeground()
        if (engine == null) startDart()
        handler.removeCallbacks(timeout)
        handler.postDelayed(timeout, 90_000)
        return START_NOT_STICKY
    }

    private fun startDart() {
        val loader = FlutterInjector.instance().flutterLoader()
        loader.startInitialization(applicationContext)
        loader.ensureInitializationComplete(applicationContext, null)
        val eng = FlutterEngine(applicationContext)
        // Register plugins (flutter_blue_plus, shared_preferences, …) on this
        // background engine so the Dart sync code can use them.
        GeneratedPluginRegistrant.registerWith(eng)
        MethodChannel(eng.dartExecutor.binaryMessenger, "esp_otp/ble_wake_bg")
            .setMethodCallHandler { call, result ->
                if (call.method == "done") {
                    result.success(null)
                    stopSelf()
                } else {
                    result.notImplemented()
                }
            }
        // 3-arg form: name the library the entrypoint lives in. The 2-arg form
        // only searches the root library (lib/main.dart); bleWakeEntrypoint is in
        // lib/sync/ble_wake.dart, so it must be addressed by its package URI or
        // the isolate fails with "Could not resolve main entrypoint function".
        val entry = DartExecutor.DartEntrypoint(
            loader.findAppBundlePath(),
            "package:esp_otp/sync/ble_wake.dart",
            "bleWakeEntrypoint"
        )
        eng.dartExecutor.executeDartEntrypoint(entry)
        engine = eng
        Log.i(TAG, "background engine started (bleWakeEntrypoint)")
    }

    private fun startInForeground() {
        val notif = buildNotification()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(
                NOTIF_ID, notif, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
            )
        } else {
            startForeground(NOTIF_ID, notif)
        }
    }

    private fun buildNotification(): Notification {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val mgr = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            mgr.createNotificationChannel(
                NotificationChannel(CHANNEL, "esp-otp wake sync",
                    NotificationManager.IMPORTANCE_LOW)
            )
        }
        val builder = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Notification.Builder(this, CHANNEL)
        } else {
            @Suppress("DEPRECATION") Notification.Builder(this)
        }
        return builder
            .setContentTitle("esp-otp")
            .setContentText("Syncing device time…")
            .setSmallIcon(android.R.drawable.stat_notify_sync)
            .setOngoing(true)
            .build()
    }

    override fun onDestroy() {
        handler.removeCallbacks(timeout)
        engine?.destroy()
        engine = null
        super.onDestroy()
    }

    companion object {
        private const val TAG = "SyncWakeService"
        private const val CHANNEL = "esp_otp_wake"
        private const val NOTIF_ID = 4271
    }
}
