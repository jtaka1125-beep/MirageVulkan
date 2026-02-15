package com.mirage.accessory.boot

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.provider.Settings
import android.util.Log

/**
 * Enables WiFi ADB automatically on device boot.
 *
 * Pre-requisite (one-time, via ADB):
 *   adb shell pm grant com.mirage.accessory android.permission.WRITE_SECURE_SETTINGS
 *
 * Strategy:
 *   1. Settings.Global "adb_wifi_enabled" = 1  → Android 11+ wireless debugging
 *   2. setprop + adbd restart                  → Classic tcpip (root only)
 */
class BootReceiver : BroadcastReceiver() {

    companion object {
        private const val TAG = "MirageBoot"
        private const val ADB_TCP_PORT = 5555
        private val ACCEPTED_ACTIONS = setOf(
            Intent.ACTION_BOOT_COMPLETED,
            "android.intent.action.QUICKBOOT_POWERON",
            "com.mirage.accessory.TEST_BOOT"  // for testing
        )
    }

    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action !in ACCEPTED_ACTIONS) return
        Log.i(TAG, "Received: ${intent.action}")

        Thread {
            try {
                if (intent.action == Intent.ACTION_BOOT_COMPLETED ||
                    intent.action == "android.intent.action.QUICKBOOT_POWERON") {
                    Thread.sleep(5000)
                }
                enableWiFiAdb(context)
            } catch (e: Exception) {
                Log.e(TAG, "Failed", e)
            }
        }.start()
    }

    private fun enableWiFiAdb(context: Context) {
        // Method 1: Android 11+ wireless debugging
        try {
            val ok = Settings.Global.putInt(
                context.contentResolver, "adb_wifi_enabled", 1
            )
            Log.i(TAG, "adb_wifi_enabled -> 1: $ok")
        } catch (e: SecurityException) {
            Log.e(TAG, "WRITE_SECURE_SETTINGS not granted!", e)
        }

        // Method 2: Classic tcpip (root only, best-effort)
        try {
            val cmds = arrayOf(
                "setprop service.adb.tcp.port $ADB_TCP_PORT",
                "stop adbd",
                "start adbd"
            )
            for (cmd in cmds) {
                val p = Runtime.getRuntime().exec(arrayOf("sh", "-c", cmd))
                val rc = p.waitFor()
                Log.d(TAG, "$cmd -> $rc")
                if (cmd.contains("stop")) Thread.sleep(500)
            }
        } catch (e: Exception) {
            Log.d(TAG, "Classic tcpip failed (non-root expected)")
        }
    }
}
