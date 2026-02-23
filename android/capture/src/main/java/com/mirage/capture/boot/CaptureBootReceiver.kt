package com.mirage.capture.boot

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log
import com.mirage.capture.svc.WatchdogService

/**
 * Starts WatchdogService on device boot.
 *
 * Note: ScreenCaptureService cannot be auto-started because MediaProjection requires
 * explicit user consent each boot (createScreenCaptureIntent dialog).
 * WatchdogService will post a notification prompting the user to re-open CaptureActivity.
 *
 * Pre-requisite: RECEIVE_BOOT_COMPLETED permission in Manifest (already added).
 * Test: adb shell am broadcast -a com.mirage.capture.TEST_BOOT -p com.mirage.capture
 */
class CaptureBootReceiver : BroadcastReceiver() {

    companion object {
        private const val TAG = "MirageCaptureBoot"
        private val ACCEPTED_ACTIONS = setOf(
            Intent.ACTION_BOOT_COMPLETED,
            "android.intent.action.QUICKBOOT_POWERON",
            "com.mirage.capture.TEST_BOOT"
        )
    }

    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action !in ACCEPTED_ACTIONS) return
        Log.i(TAG, "Boot received: ${intent.action}")

        Thread {
            try {
                // Brief delay for system to settle (skip for test broadcast)
                if (intent.action != "com.mirage.capture.TEST_BOOT") {
                    Thread.sleep(6000)
                }
                // WatchdogService is a plain background service (no MediaProjection needed)
                context.startService(Intent(context, WatchdogService::class.java))
                Log.i(TAG, "WatchdogService started from boot")
            } catch (e: Exception) {
                Log.e(TAG, "Failed to start WatchdogService", e)
            }
        }.start()
    }
}
