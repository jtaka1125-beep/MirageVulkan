package com.mirage.capture.svc

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.util.Log
import com.mirage.capture.capture.ScreenCaptureService
import com.mirage.capture.ui.CaptureActivity
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

/**
 * Monitors ScreenCaptureService health. Started at boot by CaptureBootReceiver.
 * Posts a tap-to-restart notification if capture is not running.
 *
 * Runs as a background service (no foreground type needed) — Android may kill it
 * during memory pressure, but boot receiver will re-create it on next reboot.
 */
class WatchdogService : Service() {

    companion object {
        private const val TAG = "MirageWatchdog"
        private const val CHANNEL_ID = "mirage_watchdog_channel"
        private const val ALERT_NOTIFICATION_ID = 2003
        private const val CHECK_INTERVAL_SEC = 20L
        private const val ALERT_AFTER_MISSED = 2
    }

    private val exec = Executors.newSingleThreadScheduledExecutor()
    private var missedCount = 0
    private var alertShown = false

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
        Log.i(TAG, "Watchdog started")

        exec.scheduleAtFixedRate({
            checkCaptureHealth()
        }, CHECK_INTERVAL_SEC, CHECK_INTERVAL_SEC, TimeUnit.SECONDS)
    }

    private fun checkCaptureHealth() {
        val isRunning = ScreenCaptureService.instance != null
        if (isRunning) {
            if (alertShown) {
                Log.i(TAG, "Capture restored after $missedCount missed checks")
                clearAlert()
            }
            missedCount = 0
        } else {
            missedCount++
            Log.d(TAG, "Capture not running (missed=$missedCount)")
            if (missedCount >= ALERT_AFTER_MISSED && !alertShown) {
                showRestartNotification()
            }
        }
    }

    private fun showRestartNotification() {
        alertShown = true
        Log.i(TAG, "Notifying user to restart capture")
        val nm = getSystemService(NotificationManager::class.java) ?: return
        val pi = PendingIntent.getActivity(
            this, 0,
            Intent(this, CaptureActivity::class.java).apply {
                flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP
            },
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        val n = android.app.Notification.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.stat_sys_warning)
            .setContentTitle("Mirage Capture stopped")
            .setContentText("Tap to restart screen capture")
            .setContentIntent(pi)
            .setAutoCancel(true)
            .build()
        nm.notify(ALERT_NOTIFICATION_ID, n)
    }

    private fun clearAlert() {
        alertShown = false
        getSystemService(NotificationManager::class.java)?.cancel(ALERT_NOTIFICATION_ID)
    }

    override fun onDestroy() {
        exec.shutdownNow()
        try { exec.awaitTermination(1, TimeUnit.SECONDS) } catch (_: InterruptedException) {}
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val chan = NotificationChannel(
                CHANNEL_ID, "Mirage Watchdog", NotificationManager.IMPORTANCE_DEFAULT
            )
            getSystemService(NotificationManager::class.java)?.createNotificationChannel(chan)
        }
    }
}
