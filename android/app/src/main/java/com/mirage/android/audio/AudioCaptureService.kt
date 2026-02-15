package com.mirage.android.audio

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
import com.mirage.android.util.parcelableExtra
import java.io.OutputStream
import java.util.concurrent.atomic.AtomicBoolean

/**
 * AudioCaptureService - Captures system audio and sends via USB AOA
 *
 * Uses AudioPlaybackCapture API (Android 10+) to capture system audio,
 * encodes with Opus codec, and sends via USB Bulk transfer.
 */
@RequiresApi(Build.VERSION_CODES.Q)
class AudioCaptureService : Service() {

    companion object {
        private const val TAG = "AudioCapture"
        private const val CHANNEL_ID = "audio_capture_channel"
        private const val NOTIFICATION_ID = 3001

        const val EXTRA_RESULT_CODE = "result_code"
        const val EXTRA_RESULT_DATA = "result_data"

        // Audio configuration
        const val SAMPLE_RATE = 48000
        const val CHANNEL_CONFIG = AudioFormat.CHANNEL_IN_STEREO
        const val AUDIO_FORMAT = AudioFormat.ENCODING_PCM_16BIT
        const val FRAME_SIZE_MS = 20 // 20ms frames for Opus
        const val SAMPLES_PER_FRAME = SAMPLE_RATE * FRAME_SIZE_MS / 1000 // 960 samples
        const val BYTES_PER_FRAME = SAMPLES_PER_FRAME * 2 * 2 // stereo 16-bit = 4 bytes/sample

        @Volatile
        var usbOutputStream: OutputStream? = null
    }

    private var mediaProjection: MediaProjection? = null
    private var audioRecord: AudioRecord? = null
    private var opusEncoder: OpusEncoder? = null
    private val isCapturing = AtomicBoolean(false)
    private var captureThread: Thread? = null
    private var audioSeq = 0  // Will wrap around naturally - protocol uses UInt32

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val notification = createNotification()
        startForeground(NOTIFICATION_ID, notification)

        // Prevent duplicate start if already capturing
        if (isCapturing.get()) {
            Log.i(TAG, "Already capturing, ignoring duplicate start")
            return START_NOT_STICKY
        }

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
        val channel = NotificationChannel(
            CHANNEL_ID,
            "Audio Capture",
            NotificationManager.IMPORTANCE_LOW
        ).apply {
            description = "Audio capture for Mirage"
        }
        val manager = getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(channel)
    }

    private fun createNotification(): Notification {
        return Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("Mirage Audio")
            .setContentText("Capturing system audio")
            .setSmallIcon(android.R.drawable.ic_btn_speak_now)
            .build()
    }

    private val projectionCallback = object : MediaProjection.Callback() {
        override fun onStop() {
            Log.i(TAG, "MediaProjection stopped by system")
            Handler(Looper.getMainLooper()).post {
                stopSelf()
            }
        }
    }

    private fun startCapture(resultCode: Int, resultData: Intent) {
        val mpm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        mediaProjection = mpm.getMediaProjection(resultCode, resultData)

        if (mediaProjection == null) {
            Log.e(TAG, "Failed to get MediaProjection")
            stopSelf()
            return
        }

        // Register callback for MediaProjection stop events
        mediaProjection?.registerCallback(projectionCallback, Handler(Looper.getMainLooper()))

        // Initialize Opus encoder
        opusEncoder = OpusEncoder(SAMPLE_RATE, 2) // stereo
        if (!opusEncoder!!.init()) {
            Log.e(TAG, "Failed to initialize Opus encoder")
            stopSelf()
            return
        }

        // Configure audio capture
        val captureConfig = AudioPlaybackCaptureConfiguration.Builder(mediaProjection!!)
            .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
            .addMatchingUsage(AudioAttributes.USAGE_GAME)
            .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
            .build()

        val audioFormat = AudioFormat.Builder()
            .setEncoding(AUDIO_FORMAT)
            .setSampleRate(SAMPLE_RATE)
            .setChannelMask(CHANNEL_CONFIG)
            .build()

        val bufferSize = AudioRecord.getMinBufferSize(SAMPLE_RATE, CHANNEL_CONFIG, AUDIO_FORMAT)
        val actualBufferSize = maxOf(bufferSize, BYTES_PER_FRAME * 4)

        try {
            audioRecord = AudioRecord.Builder()
                .setAudioPlaybackCaptureConfig(captureConfig)
                .setAudioFormat(audioFormat)
                .setBufferSizeInBytes(actualBufferSize)
                .build()
        } catch (e: SecurityException) {
            Log.e(TAG, "SecurityException creating AudioRecord", e)
            stopSelf()
            return
        }

        if (audioRecord?.state != AudioRecord.STATE_INITIALIZED) {
            Log.e(TAG, "AudioRecord not initialized")
            stopSelf()
            return
        }

        isCapturing.set(true)
        audioRecord?.startRecording()

        captureThread = Thread {
            captureLoop()
        }.apply {
            name = "AudioCaptureThread"
            start()
        }

        Log.i(TAG, "Audio capture started")
    }

    private fun captureLoop() {
        val pcmBuffer = ShortArray(SAMPLES_PER_FRAME * 2) // stereo
        val opusBuffer = ByteArray(4000) // Max Opus frame size
        var consecutiveErrors = 0
        val maxConsecutiveErrors = 10

        while (isCapturing.get()) {
            try {
                val record = audioRecord
                if (record == null || record.state != AudioRecord.STATE_INITIALIZED) {
                    Log.e(TAG, "AudioRecord not available")
                    break
                }

                val samplesRead = record.read(pcmBuffer, 0, pcmBuffer.size, AudioRecord.READ_BLOCKING)

                when {
                    samplesRead > 0 -> {
                        consecutiveErrors = 0  // Reset on success
                        // Encode to Opus
                        val encodedSize = opusEncoder?.encode(pcmBuffer, samplesRead, opusBuffer) ?: -1

                        if (encodedSize > 0) {
                            val opusData = opusBuffer.copyOf(encodedSize)
                            sendAudioFrame(opusData)
                        }
                    }
                    samplesRead == 0 -> {
                        // No data available, brief sleep to prevent busy loop
                        Thread.sleep(1)
                    }
                    samplesRead == AudioRecord.ERROR_INVALID_OPERATION -> {
                        Log.e(TAG, "AudioRecord: ERROR_INVALID_OPERATION")
                        consecutiveErrors++
                    }
                    samplesRead == AudioRecord.ERROR_BAD_VALUE -> {
                        Log.e(TAG, "AudioRecord: ERROR_BAD_VALUE")
                        consecutiveErrors++
                    }
                    samplesRead == AudioRecord.ERROR_DEAD_OBJECT -> {
                        Log.e(TAG, "AudioRecord: ERROR_DEAD_OBJECT - stopping capture")
                        break
                    }
                    else -> {
                        Log.e(TAG, "AudioRecord read error: $samplesRead")
                        consecutiveErrors++
                    }
                }

                if (consecutiveErrors >= maxConsecutiveErrors) {
                    Log.e(TAG, "Too many consecutive errors, stopping capture")
                    break
                }
            } catch (e: InterruptedException) {
                Log.i(TAG, "Capture thread interrupted")
                break
            } catch (e: Exception) {
                Log.e(TAG, "Exception in capture loop", e)
                consecutiveErrors++
                if (consecutiveErrors >= maxConsecutiveErrors) {
                    break
                }
            }
        }

        Log.i(TAG, "Capture loop ended")
    }

    private var noUsbWarningCount = 0
    private var lastUsbWarningTime = 0L
    private var usbErrorCount = 0
    private val maxUsbErrors = 10

    private fun sendAudioFrame(opusData: ByteArray) {
        val outputStream = usbOutputStream
        if (outputStream == null) {
            // Log warning periodically (every 5 seconds)
            val now = System.currentTimeMillis()
            if (now - lastUsbWarningTime > 5000) {
                noUsbWarningCount++
                Log.w(TAG, "USB not connected, dropping audio frame (count: $noUsbWarningCount)")
                lastUsbWarningTime = now
            }
            return
        }

        try {
            val timestamp = (System.currentTimeMillis() and 0xFFFFFFFF).toInt()
            val packet = com.mirage.android.usb.Protocol.buildAudioFrame(
                seq = audioSeq++,
                timestamp = timestamp,
                opusData = opusData
            )

            synchronized(outputStream) {
                outputStream.write(packet)
                outputStream.flush()
            }

            // Reset warning counter on successful send
            if (noUsbWarningCount > 0) {
                Log.i(TAG, "USB connection restored, resuming audio transmission")
                noUsbWarningCount = 0
            }
            usbErrorCount = 0
        } catch (e: java.io.IOException) {
            usbErrorCount++
            Log.e(TAG, "USB IO error (count: $usbErrorCount)", e)
            if (usbErrorCount >= maxUsbErrors) {
                Log.e(TAG, "Too many USB errors, clearing output stream")
                usbOutputStream = null
                usbErrorCount = 0
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to send audio frame", e)
        }
    }

    private fun stopCapture() {
        isCapturing.set(false)

        captureThread?.interrupt()
        try {
            captureThread?.join(1000)
        } catch (e: InterruptedException) {
            // Ignore
        }
        captureThread = null

        try {
            audioRecord?.stop()
        } catch (e: IllegalStateException) {
            Log.w(TAG, "AudioRecord already stopped")
        }
        audioRecord?.release()
        audioRecord = null

        opusEncoder?.release()
        opusEncoder = null

        try {
            mediaProjection?.unregisterCallback(projectionCallback)
        } catch (e: Exception) {
            // Ignore if already unregistered
        }
        mediaProjection?.stop()
        mediaProjection = null

        Log.i(TAG, "Audio capture stopped")
    }
}
