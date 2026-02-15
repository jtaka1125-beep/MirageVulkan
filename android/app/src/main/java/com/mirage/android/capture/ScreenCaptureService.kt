package com.mirage.android.capture

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
import com.mirage.android.usb.AccessoryIoService
import java.io.OutputStream

/**
 * Foreground service that captures screen via MediaProjection,
 * encodes to H.264, and streams via UDP or USB.
 *
 * Supports three modes:
 * - UDP mode (default): Streams to PC via WiFi
 * - USB mode: Streams to PC via USB AOA with VID0 framing
 * - TCP mode: Streams to PC via ADB forward (TCP server on localhost)
 */
class ScreenCaptureService : Service() {

    companion object {
        private const val TAG = "MirageCapture"
        private const val CHANNEL_ID = "mirage_capture_channel"
        private const val NOTIFICATION_ID = 2001

        const val EXTRA_RESULT_CODE = "resultCode"
        const val EXTRA_RESULT_DATA = "data"
        const val EXTRA_HOST = "host"
        const val EXTRA_PORT = "port"
        const val EXTRA_MIRROR_MODE = "mirror_mode"
        const val EXTRA_TCP_PORT = "tcp_port"

        /** Mirror mode: UDP over WiFi (default) */
        const val MIRROR_MODE_UDP = "udp"
        /** Mirror mode: USB AOA */
        const val MIRROR_MODE_USB = "usb"
        /** Mirror mode: TCP via ADB forward */
        const val MIRROR_MODE_TCP = "tcp"

        const val DEFAULT_TCP_PORT = 50100

        /** Singleton instance for external access */
        @Volatile
        var instance: ScreenCaptureService? = null
            private set
    }

    private var projection: MediaProjection? = null
    private var encoder: H264Encoder? = null
    private var videoSender: VideoSender? = null
    private var mirrorMode: String = MIRROR_MODE_UDP

    // Store host/port for fallback
    private var lastHost: String = "192.168.0.2"
    private var lastPort: Int = 50000

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        instance = this
        createNotificationChannel()
        startForeground(NOTIFICATION_ID, buildNotification())
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

        val mgr = getSystemService(MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        projection = mgr.getMediaProjection(resultCode, data)

        if (projection == null) {
            Log.e(TAG, "Failed to get MediaProjection")
            stopSelf()
            return START_NOT_STICKY
        }

        // Create appropriate video sender based on mode
        val tcpPort = intent?.getIntExtra(EXTRA_TCP_PORT, DEFAULT_TCP_PORT) ?: DEFAULT_TCP_PORT
        videoSender = when (mirrorMode) {
            MIRROR_MODE_USB -> {
                val usbStream = AccessoryIoService.getVideoOutputStream()
                if (usbStream != null) {
                    Log.i(TAG, "Starting USB capture via AOA")
                    UsbVideoSender(usbStream)
                } else {
                    Log.w(TAG, "USB stream not available, falling back to UDP")
                    UdpVideoSender(lastHost, lastPort)
                }
            }
            MIRROR_MODE_TCP -> {
                Log.i(TAG, "Starting TCP capture on localhost:$tcpPort")
                TcpVideoSender(tcpPort)
            }
            else -> {
                Log.i(TAG, "Starting UDP capture to $lastHost:$lastPort")
                UdpVideoSender(lastHost, lastPort)
            }
        }

        encoder = H264Encoder(this, projection!!, videoSender!!)
        encoder?.start()

        return START_STICKY
    }

    override fun onDestroy() {
        Log.i(TAG, "Stopping capture (mode=$mirrorMode)")
        instance = null
        encoder?.stop()
        projection?.stop()
        videoSender?.close()
        super.onDestroy()
    }

    /**
     * Attach USB output stream for video streaming.
     * Called by AccessoryIoService when USB is connected.
     */
    fun attachUsbStream(outputStream: OutputStream) {
        if (videoSender is UsbVideoSender) {
            Log.i(TAG, "USB stream already attached")
            return
        }

        // If currently using UDP, switch to USB
        if (videoSender != null && encoder != null && projection != null) {
            Log.i(TAG, "Switching from UDP to USB mode")
            encoder?.stop()
            videoSender?.close()

            videoSender = UsbVideoSender(outputStream)
            mirrorMode = MIRROR_MODE_USB
            encoder = H264Encoder(this, projection!!, videoSender!!)
            encoder?.start()
        }
    }

    /**
     * Detach USB output stream (USB disconnected).
     * Will fall back to UDP if host/port are available.
     */
    fun detachUsbStream() {
        if (videoSender !is UsbVideoSender) return

        Log.i(TAG, "USB disconnected, stopping video")
        encoder?.stop()
        videoSender?.close()
        videoSender = null
        encoder = null
        mirrorMode = MIRROR_MODE_UDP
    }

    /**
     * Update target FPS for the encoder.
     * Called by AccessoryIoService when CMD_VIDEO_FPS is received.
     */
    fun updateFps(targetFps: Int) {
        Log.i(TAG, "Updating FPS to $targetFps")
        encoder?.updateTargetFps(targetFps)
    }

    /**
     * Request immediate IDR frame from encoder (for packet loss recovery).
     */
    fun requestIdr() {
        Log.i(TAG, "Requesting IDR frame")
        encoder?.requestIdr()
    }

    /**
     * Get current target FPS.
     */
    fun getCurrentFps(): Int = encoder?.getTargetFps() ?: 30

    /**
     * Switch video sender mode (hot-swap).
     * Called by AccessoryIoService when CMD_VIDEO_ROUTE is received.
     *
     * @param mode MIRROR_MODE_USB or MIRROR_MODE_UDP
     * @param host Target host for UDP mode (ignored for USB)
     * @param port Target port for UDP mode (ignored for USB)
     */
    fun switchSender(mode: String, host: String? = null, port: Int = 0) {
        if (mode == mirrorMode) {
            Log.d(TAG, "Already in $mode mode, ignoring switch")
            return
        }

        val newSender: VideoSender? = when (mode) {
            MIRROR_MODE_USB -> {
                val usbStream = AccessoryIoService.getVideoOutputStream()
                if (usbStream != null) {
                    UsbVideoSender(usbStream)
                } else {
                    Log.w(TAG, "Cannot switch to USB: stream not available")
                    null
                }
            }
            MIRROR_MODE_UDP -> {
                val h = host ?: lastHost
                val p = if (port > 0) port else lastPort
                if (host != null) lastHost = host
                if (port > 0) lastPort = port
                UdpVideoSender(h, p)
            }
            MIRROR_MODE_TCP -> {
                val p = if (port > 0) port else DEFAULT_TCP_PORT
                TcpVideoSender(p)
            }
            else -> {
                Log.w(TAG, "Unknown mirror mode: $mode")
                null
            }
        }

        if (newSender != null) {
            // Hot-swap the sender in the encoder
            encoder?.switchSender(newSender)
            videoSender = newSender
            mirrorMode = mode
            Log.i(TAG, "Switched to $mode mode")
        }
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "Mirage Screen Capture",
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "Screen capture for mirroring"
            }
            val nm = getSystemService(NotificationManager::class.java)
            nm.createNotificationChannel(channel)
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
