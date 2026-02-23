package com.mirage.capture.capture

import android.app.Activity
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.IBinder
import android.util.Log
import androidx.core.app.NotificationCompat
import com.mirage.capture.ipc.AccessoryCommandReceiver
import java.io.OutputStream

/**
 * Foreground service: screen capture -> H.264 -> UDP/TCP/USB streaming.
 */
class ScreenCaptureService : Service() {

    companion object {
        private const val TAG = "MirageCapture"
        private const val CHANNEL_ID = "mirage_capture_channel"
        private const val NOTIFICATION_ID = 2001
        private const val TCP_SECONDARY_PORT = 50100

        const val EXTRA_RESULT_CODE = "resultCode"
        const val EXTRA_RESULT_DATA = "data"
        const val EXTRA_HOST = "host"
        const val EXTRA_PORT = "port"
        const val EXTRA_MIRROR_MODE = "mirror_mode"
        const val EXTRA_TCP_PORT = "tcp_port"

        const val MIRROR_MODE_UDP = "udp"
        const val MIRROR_MODE_USB = "usb"
        const val MIRROR_MODE_TCP = "tcp"

        const val DEFAULT_TCP_PORT = 50100
        const val ACTION_SET_FPS = "com.mirage.capture.SET_FPS"

        @Volatile
        var instance: ScreenCaptureService? = null
            private set
    }

    private var projection: MediaProjection? = null
    private var encoder: H264Encoder? = null
    private var videoSender: VideoSender? = null
    private var tcpSecondarySender: TcpVideoSender? = null

    // Public getter for CaptureActivity status display
    var mirrorMode: String = MIRROR_MODE_UDP
        private set

    private var lastHost: String = "192.168.0.2"
    private var lastPort: Int = 50000

    private var ipcReceiver: AccessoryCommandReceiver? = null

    private val fpsReceiver = object : android.content.BroadcastReceiver() {
        override fun onReceive(context: android.content.Context?, intent: android.content.Intent?) {
            if (intent?.action == ACTION_SET_FPS) {
                val fps = intent.getIntExtra("fps", -1)
                if (fps in 10..60) {
                    Log.i(TAG, "BroadcastReceiver: SET_FPS=$fps")
                    updateFps(fps)
                }
            }
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        instance = this
        createNotificationChannel()
        startForeground(NOTIFICATION_ID, buildNotification())

        ipcReceiver = AccessoryCommandReceiver()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(ipcReceiver, AccessoryCommandReceiver.createIntentFilter(),
                RECEIVER_EXPORTED)
        } else {
            registerReceiver(ipcReceiver, AccessoryCommandReceiver.createIntentFilter())
        }
        Log.i(TAG, "IPC receiver registered")

        val fpsFilter = android.content.IntentFilter(ACTION_SET_FPS)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(fpsReceiver, fpsFilter, RECEIVER_EXPORTED)
        } else {
            registerReceiver(fpsReceiver, fpsFilter)
        }
        Log.i(TAG, "FPS receiver registered")
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val resultCode = intent?.getIntExtra(EXTRA_RESULT_CODE, Activity.RESULT_CANCELED)
            ?: Activity.RESULT_CANCELED
        val data = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent?.getParcelableExtra(EXTRA_RESULT_DATA, Intent::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent?.getParcelableExtra(EXTRA_RESULT_DATA)
        }
        lastHost = intent?.getStringExtra(EXTRA_HOST) ?: "192.168.0.2"
        lastPort = intent?.getIntExtra(EXTRA_PORT, 50000) ?: 50000
        mirrorMode = intent?.getStringExtra(EXTRA_MIRROR_MODE) ?: MIRROR_MODE_UDP

        if (data == null || resultCode != Activity.RESULT_OK) {
            Log.e(TAG, "Invalid MediaProjection result")
            stopSelf()
            return START_NOT_STICKY
        }

        // Duplicate start guard: if already capturing, ignore re-start
        if (encoder != null && videoSender != null && videoSender!!.isActive()) {
            Log.w(TAG, "Already capturing (mode=$mirrorMode), ignoring duplicate onStartCommand")
            return START_STICKY
        }

        // Clean up previous session if sender is dead
        if (videoSender != null) {
            Log.i(TAG, "Previous sender inactive, cleaning up before restart")
            stopTcpSecondary()
            encoder?.stop()
            projection?.stop()
            videoSender?.close()
            encoder = null
            videoSender = null
            projection = null
        }

        val mgr = getSystemService(MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        projection = mgr.getMediaProjection(resultCode, data)

        if (projection == null) {
            Log.e(TAG, "Failed to get MediaProjection")
            stopSelf()
            return START_NOT_STICKY
        }

        val tcpPort = intent?.getIntExtra(EXTRA_TCP_PORT, DEFAULT_TCP_PORT) ?: DEFAULT_TCP_PORT
        videoSender = when (mirrorMode) {
            MIRROR_MODE_USB -> {
                Log.w(TAG, "USB mode requested but no stream attached, falling back to UDP")
                UdpVideoSender(lastHost, lastPort)
            }
            MIRROR_MODE_TCP -> {
                Log.i(TAG, "Starting TCP capture on localhost:$tcpPort")
                TcpVideoSender(tcpPort) { requestIdr() }
            }
            else -> {
                Log.i(TAG, "Starting UDP capture to $lastHost:$lastPort")
                UdpVideoSender(lastHost, lastPort)
            }
        }

        encoder = H264Encoder(this, projection!!, videoSender!!)
        encoder?.start()

        try {
            startTcpSecondary()
        } catch (e: Exception) {
            Log.e(TAG, "TCP secondary failed (service continues): ${e.message}")
        }

        return START_STICKY
    }

    private fun startTcpSecondary() {
        if (mirrorMode == MIRROR_MODE_TCP) {
            Log.i(TAG, "Primary is TCP, skipping secondary")
            return
        }
        try {
            val tcpSender = TcpVideoSender(TCP_SECONDARY_PORT) {
                encoder?.requestIdr()
            }
            tcpSecondarySender = tcpSender
            encoder?.addSecondarySender(tcpSender)
            Log.i(TAG, "TCP secondary sender started on port $TCP_SECONDARY_PORT (for GUI)")
        } catch (e: Exception) {
            Log.w(TAG, "Failed to start TCP secondary sender: ${e.message}")
        }
    }

    private fun stopTcpSecondary() {
        tcpSecondarySender?.let {
            encoder?.removeSecondarySender(it)
            tcpSecondarySender = null
            Log.i(TAG, "TCP secondary sender stopped")
        }
    }


    override fun onDestroy() {
        Log.i(TAG, "Stopping capture (mode=$mirrorMode)")
        instance = null
        try { unregisterReceiver(fpsReceiver) } catch (_: Exception) {}
        try { ipcReceiver?.let { unregisterReceiver(it) } } catch (_: Exception) {}
        ipcReceiver = null
        stopTcpSecondary()
        encoder?.stop()
        projection?.stop()
        videoSender?.close()
        super.onDestroy()
    }

    fun attachUsbStream(outputStream: OutputStream) {
        if (videoSender is UsbVideoSender) return
        if (encoder != null && projection != null) {
            Log.i(TAG, "Switching to USB mode")
            stopTcpSecondary()
            encoder?.stop()
            videoSender?.close()
            videoSender = UsbVideoSender(outputStream)
            mirrorMode = MIRROR_MODE_USB
            encoder = H264Encoder(this, projection!!, videoSender!!)
            encoder?.start()
            startTcpSecondary()
        }
    }

    fun detachUsbStream() {
        if (videoSender !is UsbVideoSender) return
        Log.i(TAG, "USB disconnected, stopping video")
        stopTcpSecondary()
        encoder?.stop()
        videoSender?.close()
        videoSender = null
        encoder = null
        mirrorMode = MIRROR_MODE_UDP
    }

    fun updateFps(targetFps: Int) {
        Log.i(TAG, "Updating FPS to $targetFps")
        encoder?.updateTargetFps(targetFps)
    }

    fun requestIdr() {
        Log.i(TAG, "Requesting IDR frame")
        encoder?.requestIdr()
    }

    fun getCurrentFps(): Int = encoder?.getTargetFps() ?: 30

    fun switchSender(mode: String, host: String? = null, port: Int = 0) {
        if (mode == mirrorMode) return
        val newSender: VideoSender? = when (mode) {
            MIRROR_MODE_USB -> { Log.w(TAG, "USB switch requires external stream"); null }
            MIRROR_MODE_UDP -> {
                val h = host ?: lastHost; val p = if (port > 0) port else lastPort
                if (host != null) lastHost = host; if (port > 0) lastPort = port
                UdpVideoSender(h, p)
            }
            MIRROR_MODE_TCP -> TcpVideoSender(if (port > 0) port else DEFAULT_TCP_PORT) { requestIdr() }
            else -> null
        }
        if (newSender != null) {
            stopTcpSecondary()
            encoder?.switchSender(newSender)
            videoSender = newSender
            mirrorMode = mode
            Log.i(TAG, "Switched to $mode mode")
            startTcpSecondary()
        }
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(CHANNEL_ID, "Mirage Screen Capture", NotificationManager.IMPORTANCE_LOW)
            getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
        }
    }

    private fun buildNotification(): Notification {
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("Mirage Capture")
            .setContentText("Screen capture active")
            .setSmallIcon(android.R.drawable.presence_video_online)
            .setOngoing(true)
            .build()
    }
}
