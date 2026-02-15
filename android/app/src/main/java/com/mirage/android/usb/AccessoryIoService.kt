package com.mirage.android.usb

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
import androidx.localbroadcastmanager.content.LocalBroadcastManager
import com.mirage.android.audio.AudioCaptureService
import com.mirage.android.util.parcelableExtra
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.IOException
import java.io.OutputStream
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Foreground service that handles USB accessory I/O.
 * Reads commands from PC, dispatches to AccessibilityService, sends ACKs.
 * Also provides synchronized output stream for video streaming.
 */
class AccessoryIoService : Service() {
    companion object {
        private const val TAG = "MirageAccessoryIO"
        private const val CHANNEL_ID = "mirage_usb_channel"
        private const val NOTIFICATION_ID = 1001

        // Broadcast actions for AccessibilityService bridge
        const val ACTION_COMMAND = "com.mirage.android.USB_COMMAND"
        const val EXTRA_COMMAND_TYPE = "cmd_type"
        const val EXTRA_SEQ = "seq"
        const val EXTRA_X = "x"
        const val EXTRA_Y = "y"
        const val EXTRA_KEYCODE = "keycode"

        const val CMD_TYPE_PING = 0
        const val CMD_TYPE_TAP = 1
        const val CMD_TYPE_BACK = 2
        const val CMD_TYPE_KEY = 3

        /** Singleton instance for accessing the service */
        @Volatile
        var instance: AccessoryIoService? = null
            private set

        /**
         * Get synchronized output stream for video streaming.
         * Returns null if USB is not connected.
         */
        fun getVideoOutputStream(): java.io.OutputStream? {
            return instance?.videoOutputStream
        }
    }

    private var fileDescriptor: ParcelFileDescriptor? = null
    private var inputStream: FileInputStream? = null
    private var outputStream: FileOutputStream? = null
    private var ioThread: Thread? = null
    private val running = AtomicBoolean(false)
    private val starting = AtomicBoolean(false)  // Guard against rapid duplicate starts
    private val outputLock = Any()  // Lock for synchronized write access

    /** Synchronized output stream for video (shares lock with command ACKs) */
    private var videoOutputStream: SynchronizedOutputStream? = null

    override fun onCreate() {
        super.onCreate()
        instance = this
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // Guard: block ALL duplicate starts with atomic flag
        if (!starting.compareAndSet(false, true)) {
            Log.i(TAG, "Already starting/running, ignoring duplicate intent")
            return START_STICKY
        }

        // If already running IO loop, also skip
        if (running.get()) {
            Log.i(TAG, "IO loop already running, ignoring")
            starting.set(false)
            return START_STICKY
        }

        val accessory = intent?.parcelableExtra<UsbAccessory>(UsbManager.EXTRA_ACCESSORY)
        if (accessory == null) {
            Log.e(TAG, "No accessory in intent")
            starting.set(false)
            stopSelf()
            return START_NOT_STICKY
        }

        // Close any previous accessory before opening new one
        closeAccessory()

        // Android 14+ requires accessory to be opened BEFORE startForeground
        // for FOREGROUND_SERVICE_CONNECTED_DEVICE permission
        if (!openAccessory(accessory)) {
            Log.e(TAG, "Failed to open accessory")
            starting.set(false)
            stopSelf()
            return START_NOT_STICKY
        }

        startForeground(NOTIFICATION_ID, buildNotification())

        startIoLoop()

        // Send device info (hardware_id) to PC for USB/ADB mapping
        sendDeviceInfo()

        // Note: starting flag stays true until IO loop ends
        return START_STICKY
    }

    /**
     * Send hardware_id to PC so it can map USB serial <-> ADB hardware_id.
     */
    private fun sendDeviceInfo() {
        try {
            // Build hardware_id: android_id + "_" + serial (same as PC's getHardwareId)
            val androidId = android.provider.Settings.Secure.getString(
                contentResolver, android.provider.Settings.Secure.ANDROID_ID
            ) ?: ""
            val serial = android.os.Build.getSerial() ?: android.os.Build.SERIAL ?: ""
            val hardwareId = if (androidId.isNotEmpty() && serial.isNotEmpty()) {
                "${androidId}_$serial"
            } else if (androidId.isNotEmpty()) {
                androidId
            } else {
                serial
            }

            if (hardwareId.isNotEmpty()) {
                val packet = Protocol.buildDeviceInfo(hardwareId)
                outputStream?.write(packet)
                outputStream?.flush()
                Log.i(TAG, "Sent DEVICE_INFO: hardware_id=$hardwareId")
            }
        } catch (e: SecurityException) {
            Log.w(TAG, "Cannot read serial (no READ_PHONE_STATE permission), using android_id only")
            try {
                val androidId = android.provider.Settings.Secure.getString(
                    contentResolver, android.provider.Settings.Secure.ANDROID_ID
                ) ?: return
                val packet = Protocol.buildDeviceInfo(androidId)
                outputStream?.write(packet)
                outputStream?.flush()
                Log.i(TAG, "Sent DEVICE_INFO: hardware_id=$androidId (android_id only)")
            } catch (e2: Exception) {
                Log.e(TAG, "Failed to send device info", e2)
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to send device info", e)
        }
    }

    override fun onDestroy() {
        instance = null
        stopIoLoop()
        closeAccessory()

        // Notify ScreenCaptureService that USB is disconnected
        try {
            com.mirage.android.capture.ScreenCaptureService.instance?.detachUsbStream()
        } catch (e: Exception) {
            Log.w(TAG, "Failed to notify ScreenCaptureService", e)
        }

        // Stop audio capture service when USB is disconnected
        try {
            stopService(Intent(this, AudioCaptureService::class.java))
        } catch (e: Exception) {
            Log.w(TAG, "Failed to stop AudioCaptureService", e)
        }

        starting.set(false)
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun createNotificationChannel() {
        val channel = NotificationChannel(
            CHANNEL_ID,
            "Mirage USB Service",
            NotificationManager.IMPORTANCE_LOW
        ).apply {
            description = "USB accessory communication"
        }
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(channel)
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
        fileDescriptor = usbManager.openAccessory(accessory)
        if (fileDescriptor == null) {
            Log.e(TAG, "openAccessory returned null")
            return false
        }

        val fd = fileDescriptor!!.fileDescriptor
        inputStream = FileInputStream(fd)
        outputStream = FileOutputStream(fd)

        // Create synchronized output stream for shared access
        val syncOut = outputStream?.let { SynchronizedOutputStream(it, outputLock) }

        // Share with AudioCaptureService for audio transmission
        AudioCaptureService.usbOutputStream = syncOut

        // Store for video streaming
        videoOutputStream = syncOut

        Log.i(TAG, "Accessory opened: ${accessory.manufacturer}/${accessory.model}")

        // Notify ScreenCaptureService that USB is available
        syncOut?.let { stream ->
            com.mirage.android.capture.ScreenCaptureService.instance?.attachUsbStream(stream)
        }

        return true
    }

    private fun closeAccessory() {
        // Clear shared output streams before closing
        AudioCaptureService.usbOutputStream = null
        videoOutputStream = null

        try {
            inputStream?.close()
            synchronized(outputLock) {
                outputStream?.close()
            }
            fileDescriptor?.close()
        } catch (e: IOException) {
            Log.w(TAG, "Error closing accessory", e)
        }
        inputStream = null
        outputStream = null
        fileDescriptor = null
    }

    private fun startIoLoop() {
        if (running.getAndSet(true)) return

        ioThread = Thread({
            val buffer = ByteArray(1024)
            Log.i(TAG, "IO loop started")

            while (running.get()) {
                try {
                    val bytesRead = inputStream?.read(buffer) ?: -1
                    if (bytesRead < 0) {
                        Log.i(TAG, "EOF on input stream")
                        break
                    }
                    if (bytesRead == 0) continue

                    val data = buffer.copyOf(bytesRead)
                    processPacket(data)

                } catch (e: IOException) {
                    if (running.get()) {
                        Log.e(TAG, "IO error", e)
                    }
                    break
                }
            }

            Log.i(TAG, "IO loop ended")
            running.set(false)
            starting.set(false)  // Allow restart after IO loop ends
            stopSelf()
        }, "AccessoryIO")

        ioThread?.start()
    }

    private fun stopIoLoop() {
        running.set(false)
        ioThread?.interrupt()
        try {
            ioThread?.join(1000)
        } catch (e: InterruptedException) {
            // ignore
        }
        ioThread = null
    }

    private fun processPacket(data: ByteArray) {
        val cmd = Protocol.parseCommand(data)
        if (cmd == null) {
            Log.w(TAG, "Failed to parse packet (${data.size} bytes)")
            return
        }

        Log.d(TAG, "Received command: $cmd")

        val status = when (cmd) {
            is Protocol.Command.Ping -> {
                broadcastCommand(CMD_TYPE_PING, cmd.seq)
                Protocol.STATUS_OK
            }
            is Protocol.Command.Tap -> {
                broadcastCommand(CMD_TYPE_TAP, cmd.seq, cmd.x, cmd.y)
                Protocol.STATUS_OK
            }
            is Protocol.Command.Back -> {
                broadcastCommand(CMD_TYPE_BACK, cmd.seq)
                Protocol.STATUS_OK
            }
            is Protocol.Command.Key -> {
                broadcastCommand(CMD_TYPE_KEY, cmd.seq, keycode = cmd.keycode)
                Protocol.STATUS_OK
            }
            is Protocol.Command.VideoFps -> {
                handleVideoFps(cmd.targetFps)
                Protocol.STATUS_OK
            }
            is Protocol.Command.VideoRoute -> {
                handleVideoRoute(cmd.mode, cmd.host, cmd.port)
                Protocol.STATUS_OK
            }
            is Protocol.Command.VideoIdr -> {
                handleVideoIdr()
                Protocol.STATUS_OK
            }
            is Protocol.Command.Unknown -> {
                Log.w(TAG, "Unknown command: ${cmd.cmd}")
                Protocol.STATUS_ERR_UNKNOWN_CMD
            }
        }

        sendAck(cmd.seq, status)
    }

    /**
     * Handle FPS change command from PC.
     */
    private fun handleVideoFps(targetFps: Int) {
        Log.i(TAG, "Received FPS change command: $targetFps")
        try {
            com.mirage.android.capture.ScreenCaptureService.instance?.updateFps(targetFps)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to update FPS", e)
        }
    }

    /**
     * Handle IDR request from PC (for packet loss recovery).
     */
    private fun handleVideoIdr() {
        Log.i(TAG, "Received IDR request from PC")
        try {
            com.mirage.android.capture.ScreenCaptureService.instance?.requestIdr()
        } catch (e: Exception) {
            Log.e(TAG, "Failed to request IDR", e)
        }
    }

    /**
     * Handle video route switch command from PC.
     */
    private fun handleVideoRoute(mode: Int, host: String, port: Int) {
        Log.i(TAG, "Received video route command: mode=$mode host=$host port=$port")
        try {
            val modeStr = if (mode == Protocol.VIDEO_ROUTE_USB) {
                com.mirage.android.capture.ScreenCaptureService.MIRROR_MODE_USB
            } else {
                com.mirage.android.capture.ScreenCaptureService.MIRROR_MODE_UDP
            }
            com.mirage.android.capture.ScreenCaptureService.instance?.switchSender(modeStr, host, port)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to switch video route", e)
        }
    }

    private fun broadcastCommand(type: Int, seq: Int, x: Int = 0, y: Int = 0, keycode: Int = 0) {
        // Direct call to AccessibilityService (same process, more reliable than LocalBroadcast)
        val a11y = com.mirage.android.access.MirageAccessibilityService.instance
        if (a11y != null) {
            Log.i(TAG, "Direct dispatch: type=$type seq=$seq x=$x y=$y keycode=$keycode")
            when (type) {
                CMD_TYPE_PING -> Log.i(TAG, "PING seq=$seq")
                CMD_TYPE_TAP -> a11y.tap(x.toFloat(), y.toFloat(), seq)
                CMD_TYPE_BACK -> a11y.performBack(seq)
                CMD_TYPE_KEY -> Log.i(TAG, "KEY keycode=$keycode seq=$seq (TODO)")
            }
        } else {
            Log.w(TAG, "AccessibilityService not available! Falling back to LocalBroadcast. type=$type")
            val intent = Intent(ACTION_COMMAND).apply {
                putExtra(EXTRA_COMMAND_TYPE, type)
                putExtra(EXTRA_SEQ, seq)
                putExtra(EXTRA_X, x)
                putExtra(EXTRA_Y, y)
                putExtra(EXTRA_KEYCODE, keycode)
            }
            LocalBroadcastManager.getInstance(this).sendBroadcast(intent)
        }
    }

    private fun sendAck(seq: Int, status: Byte) {
        synchronized(outputLock) {
            try {
                val ack = Protocol.buildAck(seq, status)
                outputStream?.write(ack)
                outputStream?.flush()
                Log.d(TAG, "Sent ACK seq=$seq status=$status")
            } catch (e: IOException) {
                Log.e(TAG, "Failed to send ACK", e)
            }
        }
    }

    /**
     * Thread-safe OutputStream wrapper for shared access
     */
    private class SynchronizedOutputStream(
        private val inner: OutputStream,
        private val lock: Any
    ) : OutputStream() {
        override fun write(b: Int) {
            synchronized(lock) {
                inner.write(b)
            }
        }

        override fun write(b: ByteArray) {
            synchronized(lock) {
                inner.write(b)
            }
        }

        override fun write(b: ByteArray, off: Int, len: Int) {
            synchronized(lock) {
                inner.write(b, off, len)
            }
        }

        override fun flush() {
            synchronized(lock) {
                inner.flush()
            }
        }

        override fun close() {
            // Don't close the underlying stream from here
        }
    }
}
