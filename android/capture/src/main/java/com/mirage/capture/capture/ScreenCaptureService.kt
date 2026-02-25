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

        private const val ACTION_CMD = "com.mirage.capture.CMD"
        private const val EXTRA_CMD = "cmd"
        private const val CMD_SET_FPS = "set_fps"
        private const val CMD_REQUEST_IDR = "request_idr"
        private const val CMD_SET_ROUTE = "set_route"
        private const val CMD_AI_START = "ai_start"
        private const val CMD_AI_STOP = "ai_stop"
        private const val EXTRA_TARGET_FPS = "target_fps"
        private const val EXTRA_FPS_LEGACY = "fps"
        private const val EXTRA_ROUTE_MODE = "route_mode"
        private const val EXTRA_AI_PORT = "ai_port"
        private const val EXTRA_AI_WIDTH = "ai_width"
        private const val EXTRA_AI_HEIGHT = "ai_height"
        private const val EXTRA_AI_FPS = "ai_fps"
        private const val EXTRA_AI_QUALITY = "ai_quality"
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

    @Volatile private var desiredFps: Int = 30
    @Volatile private var fpsRestartPending: Boolean = false

    private var aiStream: AiStream? = null

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
        // Probe existing USB: if AccessoryIoService was already running when this service started,
        // attach to its TCP video forwarding port without waiting for ACTION_USB_CONNECTED broadcast.
        ipcReceiver?.probeExistingUsb(this)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // Direct command intents (reliable even when manifest receivers are background-limited)
        if (intent?.action == ACTION_CMD) {
            val cmd = intent.getStringExtra(EXTRA_CMD) ?: ""
            when (cmd) {
                CMD_SET_FPS -> {
                    val fps = intent.getIntExtra(EXTRA_TARGET_FPS, intent.getIntExtra(EXTRA_FPS_LEGACY, 30))
                    Log.i(TAG, "CMD: SET_FPS=$fps")
                    updateFps(fps)
                }
                CMD_REQUEST_IDR -> {
                    Log.i(TAG, "CMD: REQUEST_IDR")
                    encoder?.requestIdr()
                }
                CMD_SET_ROUTE -> {
                    val mode = intent.getIntExtra(EXTRA_ROUTE_MODE, 1)
                    val host = intent.getStringExtra(ScreenCaptureService.EXTRA_HOST) ?: ""
                    val port = intent.getIntExtra(ScreenCaptureService.EXTRA_PORT, 0)
                    Log.i(TAG, "CMD: SET_ROUTE mode=$mode host=$host port=$port")
                    updateRoute(mode, host, port)
                }
                CMD_AI_START -> {
                    val host = intent.getStringExtra(ScreenCaptureService.EXTRA_HOST) ?: ""
                    val aiPort = intent.getIntExtra(EXTRA_AI_PORT, 0)
                    val w = intent.getIntExtra(EXTRA_AI_WIDTH, 0)
                    val h = intent.getIntExtra(EXTRA_AI_HEIGHT, 0)
                    val fps = intent.getIntExtra(EXTRA_AI_FPS, 10)
                    val q = intent.getIntExtra(EXTRA_AI_QUALITY, 80)
                    Log.i(TAG, "CMD: AI_START host=$host port=$aiPort size=${w}x${h} fps=$fps q=$q")
                    val proj = projection
                    if (proj != null) {
                        if (aiStream == null) aiStream = AiStream(proj)
                        val dpi = resources.displayMetrics.densityDpi
                        aiStream?.start(host, aiPort, w, h, dpi, fps, q)
                    }
                }
                CMD_AI_STOP -> {
                    Log.i(TAG, "CMD: AI_STOP")
                    aiStream?.stop()
                    aiStream = null
                }

            }
            return START_STICKY
        }


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

        encoder = H264Encoder(this, projection!!, videoSender!!, desiredFps)
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
            encoder = H264Encoder(this, projection!!, videoSender!!, desiredFps)
            encoder?.start()
            startTcpSecondary()
        }
    }

    fun detachUsbStream() {
        if (videoSender !is UsbVideoSender) return
        val proj = projection
        if (proj == null) {
            Log.w(TAG, "USB detached but projection is null, cannot restore UDP")
            stopTcpSecondary()
            encoder?.stop()
            videoSender?.close()
            videoSender = null
            encoder = null
            mirrorMode = MIRROR_MODE_UDP
            return
        }
        Log.i(TAG, "USB disconnected, restoring UDP \u2192 $lastHost:$lastPort")
        stopTcpSecondary()
        encoder?.stop()
        videoSender?.close()
        // Restart encoder with UDP sender (projection still valid, no re-consent needed)
        val udpSender = UdpVideoSender(lastHost, lastPort)
        videoSender = udpSender
        mirrorMode = MIRROR_MODE_UDP
        encoder = H264Encoder(this, proj, udpSender, desiredFps)
        encoder?.start()
        startTcpSecondary()
        Log.i(TAG, "UDP restored: $lastHost:$lastPort")
    }

    fun updateFps(targetFps: Int) {
        val fps = targetFps.coerceIn(10, 60)
        desiredFps = fps
        Log.i(TAG, "Updating FPS to $fps")
        val proj = projection
        val sender = videoSender
        // If not capturing yet, just store desired FPS and best-effort apply.
        if (proj == null || sender == null || encoder == null) {
            encoder?.updateTargetFps(fps)
            return
        }
        // MediaCodec surface input encoders typically require reconfigure to change frame-rate.
        // We restart the encoder/virtual display with the new fps.
        restartEncoderForFps(fps)
    }

    private fun restartEncoderForFps(fps: Int) {
        if (fpsRestartPending) return
        fpsRestartPending = true
        // debounce small bursts
        android.os.Handler(android.os.Looper.getMainLooper()).postDelayed({
            try {
                val proj = projection ?: return@postDelayed
                val sender = videoSender ?: return@postDelayed
                Log.i(TAG, "Restarting encoder for FPS=$fps (mode=$mirrorMode)")
                stopTcpSecondary()
                encoder?.stop()
                // Recreate encoder with initialFps
                encoder = H264Encoder(this, proj, sender, fps)
                encoder?.start()
                startTcpSecondary()
                Log.i(TAG, "Encoder restarted for FPS=$fps")
            } catch (e: Exception) {
                Log.w(TAG, "Encoder restart failed: ${e.message}")
            } finally {
                fpsRestartPending = false
            }
        }, 350)
    }


    fun requestIdr() {
        Log.i(TAG, "Requesting IDR frame")
        encoder?.requestIdr()
    }

    fun getCurrentFps(): Int = encoder?.getTargetFps() ?: 30

    

    private fun updateRoute(routeMode: Int, host: String, port: Int) {
        // 0=USB, 1=UDP, 2=TCP (legacy GUI)
        when (routeMode) {
            0 -> switchSender(MIRROR_MODE_USB)
            2 -> switchSender(MIRROR_MODE_TCP, null, port)
            else -> switchSender(MIRROR_MODE_UDP, host, port)
        }
    }

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

