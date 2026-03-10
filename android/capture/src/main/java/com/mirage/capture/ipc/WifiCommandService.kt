package com.mirage.capture.ipc

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.util.Log

/**
 * Standalone foreground service that runs WifiCommandServer.
 * Independent of ScreenCaptureService lifecycle.
 * Starts on boot (via CaptureBootReceiver) or manually.
 */
class WifiCommandService : Service() {

    companion object {
        private const val TAG = "WifiCmdSvc"
        private const val CHANNEL_ID = "mirage_wifi_cmd_channel"
        private const val NOTIFICATION_ID = 3001

        @Volatile
        var instance: WifiCommandService? = null
            private set
    }

    override fun onCreate() {
        super.onCreate()
        instance = this
        createNotificationChannel()
        startForeground(NOTIFICATION_ID, buildNotification())
        WifiCommandServer.start()
        Log.i(TAG, "WifiCommandService started, listening on ${WifiCommandServer.cmdPort}")
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val cmdPort = intent?.getIntExtra("cmd_port", WifiCommandServer.DEFAULT_CMD_PORT)
            ?: WifiCommandServer.DEFAULT_CMD_PORT
        if (cmdPort != WifiCommandServer.cmdPort) {
            // Port changed: restart server on new port
            WifiCommandServer.stop()
            WifiCommandServer.start(cmdPort)
            Log.i(TAG, "WifiCommandService restarted on port $cmdPort")
        }
        return START_STICKY
    }

    override fun onDestroy() {
        WifiCommandServer.stop()
        instance = null
        Log.i(TAG, "WifiCommandService destroyed")
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "Mirage WiFi Command",
                NotificationManager.IMPORTANCE_LOW
            )
            getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
        }
    }

    private fun buildNotification(): Notification {
        return androidx.core.app.NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("Mirage WiFi Command")
            .setContentText("Listening on port ${WifiCommandServer.cmdPort}")
            .setSmallIcon(android.R.drawable.ic_menu_share)
            .setOngoing(true)
            .build()
    }
}
