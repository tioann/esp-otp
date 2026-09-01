package com.espotp.esp_otp

import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

/// Adds the `esp_otp/ble_wake` channel so Dart can register/cancel the OS-level
/// BLE wake scan (see BleWake.kt).
class MainActivity : FlutterActivity() {
    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, "esp_otp/ble_wake")
            .setMethodCallHandler { call, result ->
                when (call.method) {
                    "register" -> {
                        BleWake.register(applicationContext)
                        result.success(true)
                    }
                    "unregister" -> {
                        BleWake.unregister(applicationContext)
                        result.success(true)
                    }
                    "setAppSyncing" -> {
                        // The UI isolate reports whether the foreground sync
                        // service is live so the wake receiver won't arm a second
                        // autoConnect for the same device (GATT-client leak).
                        BleWake.appSyncing = call.arguments as? Boolean ?: false
                        result.success(true)
                    }
                    else -> result.notImplemented()
                }
            }
    }
}
