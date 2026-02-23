package com.mirage.capture.audio

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.Log
import androidx.annotation.RequiresApi
import com.mirage.capture.util.parcelableExtra
import java.io.OutputStream
import java.net.ServerSocket
import java.util.concurrent.atomic.AtomicBoolean

/**
 * AudioCaptureService - Captures system audio via AudioPlaybackCapture API.
 *
 * Output modes:
 * - USB: Direct write to usbOutputStream (set by MirageAccessory via IPC, future)
 * - TCP: Listens on TCP port for PC connection via ADB forward (default fallback)
 */
@RequiresApi(Build.VERSION_CODES.Q)
class AudioCaptureService : Service() {

    companion object {
        private const val TAG = "AudioCapture"
        private const val CHANNEL_ID = "audio_capture_channel"
        private const val NOTIFICATION_ID = 3001

        const val EXTRA_RESULT_CODE = "result_code"
        const val EXTRA_RESULT_DATA = "result_data"
        const val EXTRA_AUDIO_MODE = "audio_mode"
        const val AUDIO_MODE_USB = "usb"
        const val AUDIO_MODE_TCP = "tcp"
        const val DEFAULT_TCP_PORT = 50200

        const val SAMPLE_RATE = 48000
        const val CHANNEL_CONFIG = AudioFormat.CHANNEL_IN_STEREO
        const val AUDIO_FORMAT = AudioFormat.ENCODING_PCM_16BIT
        const val FRAME_SIZE_MS = 20
        const val SAMPLES_PER_FRAME = SAMPLE_RATE * FRAME_SIZE_MS / 1000
        const val BYTES_PER_FRAME = SAMPLES_PER_FRAME * 2 * 2

        /** Set by MirageAccessory via IPC (AIDL/ContentProvider) for USB mode */
        @Volatile
        var usbOutputStream: OutputStream? = null
    }

    private var mediaProjection: MediaProjection? = null
    private var audioRecord: AudioRecord? = null
    private var opusEncoder: OpusEncoder? = null
    private val isCapturing = AtomicBoolean(false)
    private var captureThread: Thread? = null
    private var audioSeq = 0

    // TCP server for ADB forward mode
    private var tcpServer: ServerSocket? = null
    private var tcpOutputStream: OutputStream? = null
    private var tcpAcceptThread: Thread? = null
    private var audioMode: String = AUDIO_MODE_TCP

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForeground(NOTIFICATION_ID, createNotification())

        if (isCapturing.get()) {
            Log.i(TAG, "Already capturing, ignoring")
            return START_NOT_STICKY
        }

        audioMode = intent?.getStringExtra(EXTRA_AUDIO_MODE) ?: AUDIO_MODE_TCP

        intent?.let {
            val resultCode = it.getIntExtra(EXTRA_RESULT_CODE, -1)
            val resultData = it.parcelableExtra<Intent>(EXTRA_RESULT_DATA)
            if (resultCode != -1 && resultData != null) {
                startCapture(resultCode, resultData)
            } else {
                Log.e(TAG, "Invalid intent data")
                stopSelf()
            }
        }

        return START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        stopCapture()
        super.onDestroy()
    }

    private fun createNotificationChannel() {
        val channel = NotificationChannel(CHANNEL_ID, "Audio Capture", NotificationManager.IMPORTANCE_LOW)
        getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
    }

    private fun createNotification(): Notification {
        return Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("Mirage Audio")
            .setContentText("Capturing system audio ($audioMode)")
            .setSmallIcon(android.R.drawable.ic_btn_speak_now)
            .build()
    }

    private val projectionCallback = object : MediaProjection.Callback() {
        override fun onStop() {
            Log.i(TAG, "MediaProjection stopped")
            Handler(Looper.getMainLooper()).post { stopSelf() }
        }
    }

    private fun startCapture(resultCode: Int, resultData: Intent) {
        val mpm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        mediaProjection = mpm.getMediaProjection(resultCode, resultData)
        if (mediaProjection == null) { Log.e(TAG, "No MediaProjection"); stopSelf(); return }

        mediaProjection?.registerCallback(projectionCallback, Handler(Looper.getMainLooper()))

        opusEncoder = OpusEncoder(SAMPLE_RATE, 2)
        if (!opusEncoder!!.init()) { Log.e(TAG, "Opus init failed"); stopSelf(); return }

        val captureConfig = AudioPlaybackCaptureConfiguration.Builder(mediaProjection!!)
            .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
            .addMatchingUsage(AudioAttributes.USAGE_GAME)
            .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
            .build()

        val audioFormat = AudioFormat.Builder()
            .setEncoding(AUDIO_FORMAT).setSampleRate(SAMPLE_RATE).setChannelMask(CHANNEL_CONFIG).build()

        val bufferSize = maxOf(AudioRecord.getMinBufferSize(SAMPLE_RATE, CHANNEL_CONFIG, AUDIO_FORMAT), BYTES_PER_FRAME * 4)

        try {
            audioRecord = AudioRecord.Builder()
                .setAudioPlaybackCaptureConfig(captureConfig)
                .setAudioFormat(audioFormat)
                .setBufferSizeInBytes(bufferSize)
                .build()
        } catch (e: SecurityException) { Log.e(TAG, "SecurityException", e); stopSelf(); return }

        if (audioRecord?.state != AudioRecord.STATE_INITIALIZED) { Log.e(TAG, "AudioRecord init failed"); stopSelf(); return }

        // Start TCP server if in TCP mode
        if (audioMode == AUDIO_MODE_TCP) {
            startTcpServer()
        }

        isCapturing.set(true)
        audioRecord?.startRecording()
        captureThread = Thread({ captureLoop() }, "AudioCaptureThread").also { it.start() }
        Log.i(TAG, "Audio capture started (mode=$audioMode)")
    }

    private fun startTcpServer() {
        tcpAcceptThread = Thread({
            try {
                tcpServer = ServerSocket(DEFAULT_TCP_PORT)
                Log.i(TAG, "TCP audio server listening on :$DEFAULT_TCP_PORT")
                while (isCapturing.get()) {
                    try {
                        val client = tcpServer?.accept() ?: break
                        Log.i(TAG, "TCP audio client connected: ${client.remoteSocketAddress}")
                        // Close previous client
                        try { tcpOutputStream?.close() } catch (_: Exception) {}
                        tcpOutputStream = client.getOutputStream()
                    } catch (e: Exception) {
                        if (isCapturing.get()) Log.w(TAG, "TCP accept error", e)
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "TCP server error", e)
            }
        }, "AudioTcpAccept").also { it.start() }
    }

    private fun captureLoop() {
        val pcmBuffer = ShortArray(SAMPLES_PER_FRAME * 2)
        val opusBuffer = ByteArray(4000)
        var errors = 0

        while (isCapturing.get()) {
            try {
                val record = audioRecord ?: break
                if (record.state != AudioRecord.STATE_INITIALIZED) break

                val n = record.read(pcmBuffer, 0, pcmBuffer.size, AudioRecord.READ_BLOCKING)
                when {
                    n > 0 -> {
                        errors = 0
                        val encoded = opusEncoder?.encode(pcmBuffer, n, opusBuffer) ?: -1
                        if (encoded > 0) sendAudioFrame(opusBuffer.copyOf(encoded))
                    }
                    n == 0 -> Thread.sleep(1)
                    n == AudioRecord.ERROR_DEAD_OBJECT -> { Log.e(TAG, "DEAD_OBJECT"); break }
                    else -> { errors++; if (errors >= 10) break }
                }
            } catch (e: InterruptedException) { break }
            catch (e: Exception) { errors++; if (errors >= 10) break }
        }
        Log.i(TAG, "Capture loop ended")
    }

    private var noOutputWarnings = 0
    private var lastWarningTime = 0L
    private var ioErrors = 0

    private fun sendAudioFrame(opusData: ByteArray) {
        // Choose output: USB if available, TCP otherwise
        val out = usbOutputStream ?: tcpOutputStream
        if (out == null) {
            val now = System.currentTimeMillis()
            if (now - lastWarningTime > 5000) {
                noOutputWarnings++
                Log.w(TAG, "No output connected, dropping audio (count: $noOutputWarnings)")
                lastWarningTime = now
            }
            return
        }

        try {
            val timestamp = (System.currentTimeMillis() and 0xFFFFFFFF).toInt()
            val packet = com.mirage.capture.usb.Protocol.buildAudioFrame(audioSeq++, timestamp, opusData)
            synchronized(out) {
                out.write(packet)
                out.flush()
            }
            if (noOutputWarnings > 0) {
                Log.i(TAG, "Output restored"); noOutputWarnings = 0
            }
            ioErrors = 0
        } catch (e: java.io.IOException) {
            ioErrors++
            if (ioErrors >= 10) {
                Log.e(TAG, "Too many IO errors, clearing output")
                if (out === usbOutputStream) usbOutputStream = null
                else tcpOutputStream = null
                ioErrors = 0
            }
        } catch (e: Exception) { Log.e(TAG, "Send error", e) }
    }

    private fun stopCapture() {
        isCapturing.set(false)
        captureThread?.interrupt()
        try { captureThread?.join(1000) } catch (_: InterruptedException) {}
        captureThread = null

        try { tcpOutputStream?.close() } catch (_: Exception) {}
        try { tcpServer?.close() } catch (_: Exception) {}
        tcpAcceptThread?.interrupt()
        tcpOutputStream = null; tcpServer = null; tcpAcceptThread = null

        try { audioRecord?.stop() } catch (_: IllegalStateException) {}
        audioRecord?.release(); audioRecord = null
        opusEncoder?.release(); opusEncoder = null
        try { mediaProjection?.unregisterCallback(projectionCallback) } catch (_: Exception) {}
        mediaProjection?.stop(); mediaProjection = null
        Log.i(TAG, "Audio capture stopped")
    }
}
