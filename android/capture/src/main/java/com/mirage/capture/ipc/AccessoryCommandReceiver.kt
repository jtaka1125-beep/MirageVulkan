package com.mirage.capture.ipc

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.util.Log
import com.mirage.capture.capture.ScreenCaptureService
import java.net.Socket

/**
 * Receives IPC broadcasts from MirageAccessory app.
 * Handles: FPS change, video route switch, IDR request, USB connect/disconnect.
 *
 * On USB_CONNECTED: connects to TCP localhost:50200 exposed by AccessoryIoService,
 * then calls ScreenCaptureService.attachUsbStream() with the socket's OutputStream.
 */
class AccessoryCommandReceiver : BroadcastReceiver() {

    // ✅ FIX-2: companion から instance 変数へ移動
    // ScreenCaptureService と同じライフサイクルで GC される
    @Volatile
    private var videoSocket: Socket? = null

    companion object {
        private const val TAG = "CaptureIPC"
        const val VIDEO_TCP_PORT = 50200

        const val ACTION_VIDEO_FPS = "com.mirage.capture.ACTION_VIDEO_FPS"
        const val ACTION_VIDEO_ROUTE = "com.mirage.capture.ACTION_VIDEO_ROUTE"
        const val ACTION_VIDEO_IDR = "com.mirage.capture.ACTION_VIDEO_IDR"
        const val ACTION_USB_CONNECTED = "com.mirage.capture.ACTION_USB_CONNECTED"
        const val ACTION_USB_DISCONNECTED = "com.mirage.capture.ACTION_USB_DISCONNECTED"

        const val EXTRA_TARGET_FPS = "target_fps"
        const val EXTRA_ROUTE_MODE = "route_mode"
        const val EXTRA_HOST = "host"
        const val EXTRA_PORT = "port"

        const val VIDEO_ROUTE_USB = 0
        const val VIDEO_ROUTE_WIFI = 1

        fun createIntentFilter(): IntentFilter {
            return IntentFilter().apply {
                addAction(ACTION_VIDEO_FPS)
                addAction(ACTION_VIDEO_ROUTE)
                addAction(ACTION_VIDEO_IDR)
                addAction(ACTION_USB_CONNECTED)
                addAction(ACTION_USB_DISCONNECTED)
            }
        }
    }

    override fun onReceive(context: Context?, intent: Intent?) {
        if (intent == null) return
        val svc = ScreenCaptureService.instance

        when (intent.action) {
            ACTION_VIDEO_FPS -> {
                val fps = intent.getIntExtra(EXTRA_TARGET_FPS, 30)
                Log.i(TAG, "FPS → $fps")
                svc?.updateFps(fps)
            }
            ACTION_VIDEO_IDR -> {
                Log.i(TAG, "IDR request")
                svc?.requestIdr()
            }
            ACTION_VIDEO_ROUTE -> {
                val mode = intent.getIntExtra(EXTRA_ROUTE_MODE, VIDEO_ROUTE_WIFI)
                val host = intent.getStringExtra(EXTRA_HOST) ?: ""
                val port = intent.getIntExtra(EXTRA_PORT, 0)
                Log.i(TAG, "Route: mode=$mode host=$host port=$port")
                val modeStr = if (mode == VIDEO_ROUTE_USB) {
                    ScreenCaptureService.MIRROR_MODE_USB
                } else {
                    ScreenCaptureService.MIRROR_MODE_UDP
                }
                svc?.switchSender(modeStr, host, port)
            }
            ACTION_USB_CONNECTED -> {
                Log.i(TAG, "USB connected → connecting video socket")
                connectVideoSocket(svc)
            }
            ACTION_USB_DISCONNECTED -> {
                Log.i(TAG, "USB disconnected")
                disconnectVideoSocket(svc)
            }
        }
    }

    /**
     * Called by ScreenCaptureService.onCreate() to detect pre-existing AccessoryIoService.
     * If AccessoryIoService is already running (port 50200 is open), we connect to it
     * without waiting for ACTION_USB_CONNECTED broadcast.
     */
    fun probeExistingUsb(svc: ScreenCaptureService) {
        // ✅ FIX-2: stale socket を sendUrgentData で実通信確認してからスキップ判定
        val existing = videoSocket
        if (existing != null && !existing.isClosed && existing.isConnected) {
            try {
                existing.sendUrgentData(0) // TCP OOB で生死確認
                Log.d(TAG, "probeExistingUsb: socket alive, skipping")
                return
            } catch (_: Exception) {
                Log.w(TAG, "probeExistingUsb: stale socket detected, reconnecting")
                try { existing.close() } catch (_: Exception) {}
                videoSocket = null
            }
        }
        // ✅ FIX-5: 固定 800ms sleep → 250ms × 8回ポーリング (最大 2秒)
        Thread({
            for (attempt in 1..8) {
                try {
                    val s = Socket("127.0.0.1", VIDEO_TCP_PORT)
                    videoSocket = s
                    Log.i(TAG, "probeExistingUsb: connected (attempt=$attempt)")
                    svc.attachUsbStream(s.getOutputStream())
                    return@Thread
                } catch (_: Exception) {
                    if (attempt < 8) Thread.sleep(250)
                }
            }
            Log.d(TAG, "probeExistingUsb: no AccessoryIoService on :$VIDEO_TCP_PORT")
        }, "UsbProbe").start()
    }

    private fun connectVideoSocket(svc: ScreenCaptureService?) {
        if (svc == null) {
            Log.w(TAG, "ScreenCaptureService not running, cannot attach USB stream")
            return
        }

        // Connect in background to avoid ANR
        Thread({
            try {
                // Retry a few times — server socket may not be ready yet
                var socket: Socket? = null
                for (attempt in 1..5) {
                    try {
                        socket = Socket("127.0.0.1", VIDEO_TCP_PORT)
                        break
                    } catch (e: Exception) {
                        socket?.close()
                        socket = null
                        if (attempt < 5) {
                            Log.i(TAG, "TCP connect attempt $attempt failed, retrying...")
                            Thread.sleep(500)
                        }
                    }
                }

                if (socket == null) {
                    Log.e(TAG, "Failed to connect to video TCP port after 5 attempts")
                    return@Thread
                }

                videoSocket = socket
                Log.i(TAG, "Video TCP socket connected to localhost:$VIDEO_TCP_PORT")
                svc.attachUsbStream(socket.getOutputStream())
            } catch (e: Exception) {
                Log.e(TAG, "Video TCP connect failed", e)
            }
        }, "VideoSocketConnect").start()
    }

    private fun disconnectVideoSocket(svc: ScreenCaptureService?) {
        svc?.detachUsbStream()
        try { videoSocket?.close() } catch (_: Exception) {}
        videoSocket = null
    }
}
