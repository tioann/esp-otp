package com.espotp.esp_otp

import android.annotation.SuppressLint
import android.app.PendingIntent
import android.bluetooth.BluetoothAdapter
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.ParcelUuid
import android.util.Log

/// Registers a hardware-offloaded BLE scan with the system, delivered via a
/// PendingIntent. The Android BLE stack watches for the esp-otp management
/// service UUID and broadcasts to BleScanReceiver when it appears — even if the
/// app process has been killed — which starts SyncWakeService to sync the clock.
object BleWake {
    private const val TAG = "BleWake"
    private const val PREFS = "esp_otp_wake"
    private const val KEY_ENABLED = "wake_enabled"
    private const val REQ = 4270

    // True while the app process is alive AND running its own foreground sync
    // (the live SyncEngine already owns an autoConnect per device). The wake
    // receiver checks this to avoid arming a *second* autoConnect for the same
    // device, which would exhaust Android's ~7 concurrent GATT clients. It is a
    // process-level flag: a process freshly spawned to deliver a wake broadcast
    // (true process death) sees the default `false` and proceeds; a live process
    // whose foreground service is running sees `true` and skips.
    @Volatile
    var appSyncing = false

    // The device advertises this 128-bit management service UUID (PROTOCOL.md).
    private val MGMT_UUID =
        ParcelUuid.fromString("6f747000-0000-1000-8000-00805f9b34fb")

    @SuppressLint("MissingPermission")
    fun register(context: Context) {
        val scanner = BluetoothAdapter.getDefaultAdapter()?.bluetoothLeScanner
        if (scanner == null) {
            Log.w(TAG, "no BLE scanner (adapter off?)")
            return
        }
        val filters = listOf(ScanFilter.Builder().setServiceUuid(MGMT_UUID).build())
        val builder = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_POWER)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            // Fire once per appearance (not on every advertisement).
            builder.setCallbackType(ScanSettings.CALLBACK_TYPE_FIRST_MATCH)
                .setMatchMode(ScanSettings.MATCH_MODE_STICKY)
        }
        try {
            scanner.startScan(filters, builder.build(), pendingIntent(context))
            setEnabled(context, true)
            Log.i(TAG, "wake scan registered")
        } catch (e: Exception) {
            Log.e(TAG, "startScan failed: ${e.message}")
        }
    }

    @SuppressLint("MissingPermission")
    fun unregister(context: Context) {
        val scanner = BluetoothAdapter.getDefaultAdapter()?.bluetoothLeScanner
        try {
            scanner?.stopScan(pendingIntent(context))
        } catch (e: Exception) {
            Log.w(TAG, "stopScan failed: ${e.message}")
        }
        setEnabled(context, false)
    }

    fun isEnabled(context: Context): Boolean =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getBoolean(KEY_ENABLED, false)

    private fun setEnabled(context: Context, on: Boolean) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit().putBoolean(KEY_ENABLED, on).apply()
    }

    private fun pendingIntent(context: Context): PendingIntent {
        val intent = Intent(context, BleScanReceiver::class.java)
            .setAction("com.espotp.esp_otp.BLE_WAKE")
        var flags = PendingIntent.FLAG_UPDATE_CURRENT
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            // The system fills scan results into the intent, so it must be mutable.
            flags = flags or PendingIntent.FLAG_MUTABLE
        }
        return PendingIntent.getBroadcast(context, REQ, intent, flags)
    }
}
