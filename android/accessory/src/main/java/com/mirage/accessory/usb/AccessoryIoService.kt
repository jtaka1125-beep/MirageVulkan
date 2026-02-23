package com.mirage.accessory.usb

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.hardware.usb.UsbAccessory
import android.hardware.usb.UsbManager
import android.os.IBinder
import android.os.ParcelFileDescriptor
import android.util.Log
import com.mirage.accessory.util.parcelableExtra
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.IOException
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Foreground service for USB AOA I/O.
 *
 * Video pipeline: MirageCapture → TCP(localhost:50200) → USB OutputStream
 * Command pipeline: USB InputStream → parse MIRA → AccessibilityService / broadcast
 *
 * Can be started in two ways:
 *   1. Normal: via USB_ACCESSORY_ATTACHED intent (UsbManager.openAccessory)
 *   2. ADB/direct: without EXTRA_ACCESSORY (opens /dev/usb_accessory directly)
 */
class AccessoryIoService : Service() {
    companion object {
        private const val TAG = "MirageAccessoryIO"
        private const val CHANNEL_ID = "mirage_usb_channel"
        private const val NOTIFICATION_ID = 1001
        const val VIDEO_TCP_PORT = 50200

        const val ACTION_COMMAND = "com.mirage.accessory.USB_COMMAND"
        const val EXTRA_COMMAND_TYPE = "cmd_type"
        const val EXTRA_SEQ = "seq"
        const val EXTRA_X = "x"
        const val EXTRA_Y = "y"
        const val EXTRA_KEYCODE = "keycode"

        const val CMD_TYPE_PING = 0
        const val CMD_TYPE_TAP = 1
        const val CMD_TYPE_BACK = 2
        const val CMD_TYPE_KEY = 3
        const val CMD_TYPE_SWIPE = 7
        const val CMD_TYPE_PINCH = 8
        const val CMD_TYPE_LONGPRESS = 9

        const val ACTION_VIDEO_FPS = "com.mirage.capture.ACTION_VIDEO_FPS"
        const val ACTION_VIDEO_ROUTE = "com.mirage.capture.ACTION_VIDEO_ROUTE"
        const val ACTION_VIDEO_IDR = "com.mirage.capture.ACTION_VIDEO_IDR"
        const val ACTION_USB_CONNECTED = "com.mirage.capture.ACTION_USB_CONNECTED"
        const val ACTION_USB_DISCONNECTED = "com.mirage.capture.ACTION_USB_DISCONNECTED"
        const val EXTRA_TARGET_FPS = "target_fps"
        const val EXTRA_ROUTE_MODE = "route_mode"
        const val EXTRA_HOST = "host"
        const val EXTRA_PORT = "port"

        private const val USB_ACCESSORY_DEV = "/dev/usb_accessory"

        @Volatile
        var instance: AccessoryIoService? = null
            private set
    }

    private var fileDescriptor: ParcelFileDescriptor? = null
    private var inputStream: FileInputStream? = null
    private var outputStream: FileOutputStream? = null
    private var ioThread: Thread? = null
    private var videoForwardThread: Thread? = null
    private var videoServerSocket: ServerSocket? = null
    private var videoClientSocket: Socket? = null
    private val running = AtomicBoolean(false)
    private val starting = AtomicBoolean(false)
    private val outputLock = Any()

    override fun onCreate() {
        super.onCreate()
        instance = this
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (!starting.compareAndSet(false, true)) {
            Log.i(TAG, "Already starting, ignoring duplicate")
            return START_STICKY
        }
        if (running.get()) {
            starting.set(false)
            return START_STICKY
        }

        val accessory = intent?.parcelableExtra<UsbAccessory>(UsbManager.EXTRA_ACCESSORY)
        // 前回接続のストリーム/FDを解放（accessoryはParcelableメタデータなので影響なし）
        closeAccessory()

        val opened = if (accessory != null) {
            Log.i(TAG, "Starting via USB_ACCESSORY_ATTACHED intent")
            openAccessory(accessory)
        } else {
            Log.i(TAG, "No accessory in intent, trying direct /dev/usb_accessory open")
            openAccessoryDirect()
        }

        if (!opened) {
            Log.e(TAG, "Failed to open accessory (both UsbManager and direct)")
            starting.set(false)
            stopSelf()
            return START_NOT_STICKY
        }

        startForeground(NOTIFICATION_ID, buildNotification())
        startIoLoop()
        startVideoForward()
        // Run on background thread to avoid ANR (USB write can block main thread)
        Thread({ sendDeviceInfo() }, "DeviceInfoSender").start()

        sendBroadcast(Intent(ACTION_USB_CONNECTED).setPackage("com.mirage.capture"))
        Log.i(TAG, "USB connected broadcast sent to capture app")

        return START_STICKY
    }

    private fun sendDeviceInfo() {
        try {
            val androidId = android.provider.Settings.Secure.getString(
                contentResolver, android.provider.Settings.Secure.ANDROID_ID
            ) ?: ""
            val serial = try { android.os.Build.getSerial() } catch (_: SecurityException) { "" }
            val hardwareId = listOf(androidId, serial).filter { it.isNotEmpty() }.joinToString("_")
            if (hardwareId.isNotEmpty()) {
                val packet = Protocol.buildDeviceInfo(hardwareId)
                synchronized(outputLock) {
                    outputStream?.write(packet)
                    outputStream?.flush()
                }
                Log.i(TAG, "Sent DEVICE_INFO: $hardwareId")
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to send device info", e)
        }
    }

    override fun onDestroy() {
        instance = null
        stopVideoForward()
        stopIoLoop()
        closeAccessory()
        sendBroadcast(Intent(ACTION_USB_DISCONNECTED).setPackage("com.mirage.capture"))
        starting.set(false)
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    // =========================================================================
    // Video forwarding: TCP(localhost:50200) → USB OutputStream
    // =========================================================================

    private fun startVideoForward() {
        stopVideoForward()
        videoForwardThread = Thread({
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_AUDIO)
            Log.i(TAG, "Video forward thread started")

            // ✅ FIX-1: ServerSocket は一度だけ作成し、再接続ループで使い回す
            try {
                videoServerSocket = ServerSocket().also {
                    it.reuseAddress = true
                    it.bind(java.net.InetSocketAddress(InetAddress.getByName("127.0.0.1"), VIDEO_TCP_PORT), 1)
                }
                Log.i(TAG, "TCP ServerSocket listening on localhost:$VIDEO_TCP_PORT")
            } catch (e: IOException) {
                Log.e(TAG, "Failed to bind ServerSocket on :$VIDEO_TCP_PORT", e)
                return@Thread
            }

            // ✅ FIX-1: 外側ループ — MirageCapture 再起動のたびに accept() を繰り返す
            while (running.get()) {
                try {
                    Log.i(TAG, "Waiting for MirageCapture on :$VIDEO_TCP_PORT...")
                    val client = videoServerSocket?.accept() ?: break
                    videoClientSocket = client
                    client.tcpNoDelay = true
                    client.receiveBufferSize = 256 * 1024
                    Log.i(TAG, "MirageCapture connected")

                    // 内側ループ: 1接続のデータ転送
                    val clientIn = client.inputStream
                    val buf = ByteArray(131072)
                    var totalBytes = 0L
                    var lastLogTime = System.currentTimeMillis()

                    while (running.get()) {
                        val n = clientIn.read(buf)
                        if (n < 0) {
                            Log.i(TAG, "MirageCapture disconnected (EOF), waiting for reconnect...")
                            break  // 外側ループに戺り次の accept() へ
                        }
                        if (n == 0) continue
                        outputStream?.write(buf, 0, n)
                        totalBytes += n
                        val now = System.currentTimeMillis()
                        if (now - lastLogTime >= 5000) {
                            Log.i(TAG, "Video fwd: ${totalBytes / 1024}KB total, last ${n}B")
                            lastLogTime = now
                        }
                    }

                    // 切断クリーンアップ (ServerSocket は維持して次の accept() へ)
                    try { videoClientSocket?.close() } catch (_: Exception) {}
                    videoClientSocket = null

                } catch (e: IOException) {
                    if (!running.get()) break
                    Log.w(TAG, "Video forward accept error: ${e.message}, retry in 1s")
                    try { Thread.sleep(1000) } catch (_: InterruptedException) { break }
                }
            }

            Log.i(TAG, "Video forward thread ended")
        }, "VideoForward").also { it.start() }
    }

    private fun stopVideoForward() {
        try { videoClientSocket?.close() } catch (_: Exception) {}
        try { videoServerSocket?.close() } catch (_: Exception) {}
        videoClientSocket = null
        videoServerSocket = null
        videoForwardThread?.interrupt()
        try { videoForwardThread?.join(1000) } catch (_: Exception) {}
        videoForwardThread = null
    }

    // =========================================================================
    // USB Accessory management
    // =========================================================================

    private fun createNotificationChannel() {
        val channel = NotificationChannel(CHANNEL_ID, "Mirage USB Service", NotificationManager.IMPORTANCE_LOW)
        getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
    }

    private fun buildNotification(): Notification {
        return Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("Mirage USB")
            .setContentText("Connected to PC")
            .setSmallIcon(android.R.drawable.ic_menu_upload)
            .setOngoing(true)
            .build()
    }

    private fun openAccessory(accessory: UsbAccessory): Boolean {
        val usbManager = getSystemService(UsbManager::class.java)
        fileDescriptor = usbManager.openAccessory(accessory) ?: run {
            Log.e(TAG, "UsbManager.openAccessory returned null, falling back to direct open")
            return openAccessoryDirect()
        }
        return try {
            val fd = fileDescriptor!!.fileDescriptor
            inputStream = FileInputStream(fd)
            outputStream = FileOutputStream(fd)
            Log.i(TAG, "Accessory opened via UsbManager: ${accessory.manufacturer}/${accessory.model}")
            true
        } catch (e: Exception) {
            // ストリーム生成失敗時はFDを閉じてリーク防止、直接オープンにフォールバック
            Log.e(TAG, "Failed to create streams from UsbManager FD, falling back to direct open", e)
            try { fileDescriptor?.close() } catch (_: Exception) {}
            fileDescriptor = null
            openAccessoryDirect()
        }
    }

    /**
     * Fallback: open /dev/usb_accessory directly (for ADB-launched service or when
     * UsbManager.openAccessory() returns null despite device being in accessory mode).
     * Requires the 'usb' group permission on /dev/usb_accessory (gid 1014, typical on AOSP).
     */
    private fun openAccessoryDirect(): Boolean {
        return try {
            val dev = File(USB_ACCESSORY_DEV)
            if (!dev.exists()) {
                Log.e(TAG, "Direct open failed: $USB_ACCESSORY_DEV does not exist")
                return false
            }
            val fis = FileInputStream(dev)
            val fos = try {
                FileOutputStream(dev)
            } catch (e: Exception) {
                // SecurityException (権限不足) や IOException をキャッチしてFISをリーク防止
                fis.close()
                throw e
            }
            inputStream = fis
            outputStream = fos
            Log.i(TAG, "Accessory opened directly via $USB_ACCESSORY_DEV")
            true
        } catch (e: Exception) {
            // IOException (デバイスI/Oエラー) + SecurityException (SELinux/権限拒否) 両方を処理
            Log.e(TAG, "Direct open of $USB_ACCESSORY_DEV failed: ${e.javaClass.simpleName}: ${e.message}")
            false
        }
    }

    private fun closeAccessory() {
        try {
            inputStream?.close()
            synchronized(outputLock) { outputStream?.close() }
            fileDescriptor?.close()
        } catch (e: IOException) { Log.w(TAG, "Error closing accessory", e) }
        inputStream = null; outputStream = null; fileDescriptor = null
    }

    // =========================================================================
    // Command I/O loop
    // =========================================================================

    private fun startIoLoop() {
        if (running.getAndSet(true)) return
        ioThread = Thread({
            val buffer = ByteArray(1024)
            Log.i(TAG, "IO loop started")
            while (running.get()) {
                try {
                    val n = inputStream?.read(buffer) ?: -1
                    if (n < 0) { Log.i(TAG, "EOF"); break }
                    if (n == 0) continue
                    processPacket(buffer.copyOf(n))
                } catch (e: IOException) {
                    if (running.get()) Log.e(TAG, "IO error", e)
                    break
                }
            }
            Log.i(TAG, "IO loop ended")
            running.set(false); starting.set(false)
            stopSelf()
        }, "AccessoryIO").also { it.start() }
    }

    private fun stopIoLoop() {
        running.set(false)
        ioThread?.interrupt()
        try { ioThread?.join(1000) } catch (_: InterruptedException) {}
        ioThread = null
    }

    private fun processPacket(data: ByteArray) {
        val cmd = Protocol.parseCommand(data) ?: return
        val status = when (cmd) {
            is Protocol.Command.Ping -> { broadcastCommand(CMD_TYPE_PING, cmd.seq); Protocol.STATUS_OK }
            is Protocol.Command.Tap -> { broadcastCommand(CMD_TYPE_TAP, cmd.seq, cmd.x, cmd.y); Protocol.STATUS_OK }
            is Protocol.Command.Back -> { broadcastCommand(CMD_TYPE_BACK, cmd.seq); Protocol.STATUS_OK }
            is Protocol.Command.Key -> { broadcastCommand(CMD_TYPE_KEY, cmd.seq, keycode = cmd.keycode); Protocol.STATUS_OK }
            is Protocol.Command.Swipe -> { handleSwipe(cmd); Protocol.STATUS_OK }
            is Protocol.Command.Pinch -> { handlePinch(cmd); Protocol.STATUS_OK }
            is Protocol.Command.LongPress -> { handleLongPress(cmd); Protocol.STATUS_OK }
            is Protocol.Command.Config -> { Log.i(TAG, "CONFIG: ${cmd.payload.size}bytes"); Protocol.STATUS_OK }
            is Protocol.Command.ClickId -> { handleClickId(cmd); Protocol.STATUS_OK }
            is Protocol.Command.ClickText -> { handleClickText(cmd); Protocol.STATUS_OK }
            is Protocol.Command.VideoFps -> { handleVideoFps(cmd.targetFps); Protocol.STATUS_OK }
            is Protocol.Command.VideoRoute -> { handleVideoRoute(cmd.mode, cmd.host, cmd.port); Protocol.STATUS_OK }
            is Protocol.Command.VideoIdr -> { handleVideoIdr(); Protocol.STATUS_OK }
            is Protocol.Command.Unknown -> Protocol.STATUS_ERR_UNKNOWN_CMD
        }
        sendAck(cmd.seq, status)
    }

    private fun handleVideoFps(fps: Int) {
        Log.i(TAG, "FPS → $fps")
        sendBroadcast(Intent(ACTION_VIDEO_FPS).setPackage("com.mirage.capture").putExtra(EXTRA_TARGET_FPS, fps))
    }
    private fun handleVideoIdr() {
        Log.i(TAG, "IDR request")
        sendBroadcast(Intent(ACTION_VIDEO_IDR).setPackage("com.mirage.capture"))
    }
    private fun handleVideoRoute(mode: Int, host: String, port: Int) {
        Log.i(TAG, "Route switch: mode=$mode host=$host port=$port")
        sendBroadcast(Intent(ACTION_VIDEO_ROUTE).setPackage("com.mirage.capture")
            .putExtra(EXTRA_ROUTE_MODE, mode).putExtra(EXTRA_HOST, host).putExtra(EXTRA_PORT, port))
    }

    private fun broadcastCommand(type: Int, seq: Int, x: Int = 0, y: Int = 0, keycode: Int = 0) {
        val a11y = com.mirage.accessory.access.MirageAccessibilityService.instance
        if (a11y != null) {
            when (type) {
                CMD_TYPE_PING -> Log.d(TAG, "PING seq=$seq")
                CMD_TYPE_TAP -> a11y.tap(x.toFloat(), y.toFloat(), seq)
                CMD_TYPE_BACK -> a11y.performBack(seq)
                CMD_TYPE_KEY -> Log.i(TAG, "KEY $keycode seq=$seq")
            }
        } else {
            Log.w(TAG, "A11y not available, type=$type")
        }
    }

    private fun handleSwipe(cmd: Protocol.Command.Swipe) {
        com.mirage.accessory.access.MirageAccessibilityService.instance
            ?.swipe(cmd.startX.toFloat(), cmd.startY.toFloat(), cmd.endX.toFloat(), cmd.endY.toFloat(), cmd.durationMs, cmd.seq)
    }
    private fun handlePinch(cmd: Protocol.Command.Pinch) {
        com.mirage.accessory.access.MirageAccessibilityService.instance
            ?.pinch(cmd.centerX.toFloat(), cmd.centerY.toFloat(), cmd.startDistance, cmd.endDistance, cmd.durationMs, cmd.angle, cmd.seq)
    }
    private fun handleLongPress(cmd: Protocol.Command.LongPress) {
        com.mirage.accessory.access.MirageAccessibilityService.instance
            ?.longPress(cmd.x.toFloat(), cmd.y.toFloat(), cmd.durationMs, cmd.seq)
    }
    private fun handleClickId(cmd: Protocol.Command.ClickId) {
        com.mirage.accessory.access.MirageAccessibilityService.instance?.clickById(cmd.resourceId, cmd.seq)
    }
    private fun handleClickText(cmd: Protocol.Command.ClickText) {
        com.mirage.accessory.access.MirageAccessibilityService.instance?.clickByText(cmd.text, cmd.seq)
    }

    private fun sendAck(seq: Int, status: Byte) {
        synchronized(outputLock) {
            try {
                outputStream?.write(Protocol.buildAck(seq, status))
                outputStream?.flush()
            } catch (e: IOException) { Log.e(TAG, "ACK fail", e) }
        }
    }
}
