package com.espotp.esp_otp

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log
import androidx.core.content.ContextCompat

/// Receives the offloaded-scan PendingIntent broadcast (delivered by the system
/// BLE stack, even when the app process was dead) and starts SyncWakeService to
/// run the on-connect clock sync.
class BleScanReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        // If the app is already alive and running its foreground sync, its live
        // SyncEngine already has an autoConnect armed for this device. Starting
        // the wake service too would arm a second one and leak GATT clients
        // (Android caps concurrent clients ~7 → GATT_CONN_FAILED_ESTABLISHMENT).
        if (BleWake.appSyncing) {
            Log.i("BleScanReceiver", "app already syncing — skip wake")
            return
        }
        Log.i("BleScanReceiver", "esp-otp advertisement seen — starting wake sync")
        try {
            ContextCompat.startForegroundService(
                context, Intent(context, SyncWakeService::class.java)
            )
        } catch (e: Exception) {
            // Android 12+ can block starting a foreground service from the
            // background in some states; nothing else we can safely do here.
            Log.w("BleScanReceiver", "startForegroundService blocked: ${e.message}")
        }
    }
}
