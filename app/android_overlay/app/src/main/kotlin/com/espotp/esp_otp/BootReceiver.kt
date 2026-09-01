package com.espotp.esp_otp

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/// Re-registers the offloaded wake scan after a reboot (the OS clears offloaded
/// scans on restart). Only re-arms if background sync was left enabled.
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action == Intent.ACTION_BOOT_COMPLETED && BleWake.isEnabled(context)) {
            BleWake.register(context)
        }
    }
}
