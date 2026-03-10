package com.mirage.capture.boot

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.provider.Settings
import android.util.Log
import com.mirage.capture.svc.WatchdogService
import com.mirage.capture.ipc.WifiCommandService

/**
 * On device boot:
 *   1. Enables WiFi ADB (port 5555) via Settings.Global + classic tcpip
 *   2. Starts WatchdogService
 *
 * Pre-requisite (one-time):
 *   adb shell pm grant com.mirage.capture android.permission.WRITE_SECURE_SETTINGS
 *
 * Test:
 *   adb shell am broadcast -a com.mirage.capture.TEST_BOOT -p com.mirage.capture
 */
class CaptureBootReceiver : BroadcastReceiver() {

    companion object {
        private const val TAG = "MirageCaptureBoot"
        private const val ADB_TCP_PORT = 5555
        private val ACCEPTED_ACTIONS = setOf(
            Intent.ACTION_BOOT_COMPLETED,
            "android.intent.action.QUICKBOOT_POWERON",
            "com.mirage.capture.TEST_BOOT",
            "com.mirage.accessory.TEST_BOOT"
        )
    }

    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action !in ACCEPTED_ACTIONS) return
        Log.i(TAG, "Boot received: ${intent.action}")

        val isTest = intent.action?.contains("TEST_BOOT") == true

        Thread {
            try {
                if (!isTest) Thread.sleep(6000)

                enableWiFiAdb(context)

                try {
                    context.startForegroundService(Intent(context, WatchdogService::class.java))
                    Log.i(TAG, "WatchdogService started from boot")
                } catch (e: Exception) {
                    Log.w(TAG, "WatchdogService start failed (non-fatal): ${e.message}")
                }

                try {
                    context.startForegroundService(Intent(context, WifiCommandService::class.java))
                    Log.i(TAG, "WifiCommandService started from boot")
                } catch (e: Exception) {
                    Log.e(TAG, "WifiCommandService start failed", e)
                }
            } catch (e: Exception) {
                Log.e(TAG, "Boot init failed", e)
            }
        }.start()
    }

    private fun enableWiFiAdb(context: Context) {
        // Method 1: Android 11+ wireless debugging via Settings.Global
        try {
            val ok = Settings.Global.putInt(context.contentResolver, "adb_wifi_enabled", 1)
            Log.i(TAG, "adb_wifi_enabled -> 1: $ok")
        } catch (e: SecurityException) {
            Log.e(TAG, "WRITE_SECURE_SETTINGS not granted! Run: adb shell pm grant com.mirage.capture android.permission.WRITE_SECURE_SETTINGS", e)
        }

        // Method 2: Classic tcpip mode (root only, best-effort)
        try {
            val cmds = arrayOf(
                "setprop service.adb.tcp.port $ADB_TCP_PORT",
                "stop adbd",
                "start adbd"
            )
            for (cmd in cmds) {
                val p = Runtime.getRuntime().exec(arrayOf("sh", "-c", cmd))
                p.waitFor()
                Log.d(TAG, "$cmd -> done")
                if (cmd.contains("stop")) Thread.sleep(500)
            }
            Log.i(TAG, "Classic tcpip port $ADB_TCP_PORT attempted")
        } catch (e: Exception) {
            Log.d(TAG, "Classic tcpip failed (non-root expected): ${e.message}")
        }
    }
}
