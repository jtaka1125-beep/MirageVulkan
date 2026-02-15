package com.mirage.android.svc

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.media.ImageReader
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.IBinder
import android.os.SystemClock
import android.util.Log
import com.mirage.android.util.parcelableExtra

class CaptureService : Service() {
    companion object {
        const val EXTRA_RESULT_CODE = "result_code"
        const val EXTRA_RESULT_DATA = "result_data"
        private const val TAG = "MirageCap"
        private const val CH_ID = "mirage_capture"
        private const val NOTIF_ID = 1001
    }

    private var projection: MediaProjection? = null
    private var reader: ImageReader? = null

    override fun onCreate() {
        super.onCreate()
        createChannel()
        startForeground(NOTIF_ID, buildNotif("Capturing (scaffold)"))
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val rc = intent?.getIntExtra(EXTRA_RESULT_CODE, -1) ?: -1
        val data = intent?.parcelableExtra<Intent>(EXTRA_RESULT_DATA)

        if (rc <= 0 || data == null) {
            Log.w(TAG, "No MediaProjection permission data. Capture disabled in scaffold.")
            return START_STICKY
        }

        // Stop previous projection if exists (prevent leak on duplicate onStartCommand)
        projection?.let { oldProjection ->
            Log.w(TAG, "Stopping previous MediaProjection before acquiring new one")
            try {
                oldProjection.stop()
            } catch (e: Exception) {
                Log.w(TAG, "Error stopping previous projection", e)
            }
        }
        reader?.let { oldReader ->
            try {
                oldReader.close()
            } catch (e: Exception) {
                Log.w(TAG, "Error closing previous reader", e)
            }
        }
        reader = null

        val mpm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        projection = mpm.getMediaProjection(rc, data)

        // NOTE: For scaffold, we only demonstrate meta timestamps; actual encoder wiring is deferred.
        // When ImageReader is wired, use Image.timestamp as cap_mono_ns and System.currentTimeMillis() as cap_wall_ms.

        Log.i(TAG, "MediaProjection acquired. (encoder not wired yet)")
        return START_STICKY
    }

    override fun onDestroy() {
        reader?.close()
        projection?.stop()
        reader = null
        projection = null
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun createChannel() {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        val ch = NotificationChannel(CH_ID, "Mirage Capture", NotificationManager.IMPORTANCE_LOW)
        nm.createNotificationChannel(ch)
    }

    private fun buildNotif(msg: String): Notification {
        return Notification.Builder(this, CH_ID)
            .setContentTitle("MirageAndroid")
            .setContentText(msg)
            .setSmallIcon(android.R.drawable.stat_sys_upload)
            .build()
    }
}
