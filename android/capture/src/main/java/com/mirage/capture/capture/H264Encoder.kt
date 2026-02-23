package com.mirage.capture.capture

import android.content.Context
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.projection.MediaProjection
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.util.DisplayMetrics
import android.util.Log
import android.view.Surface
import android.view.WindowManager
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

/**
 * H.264 encoder v5d: Direct mode (NO SurfaceRepeater).
 *
 * Pipeline:
 *   VirtualDisplay -> MediaCodec (direct Surface input) -> RTP -> VideoSender
 *
 * SurfaceRepeater was causing frame freeze: VirtualDisplay frames stopped reaching
 * SurfaceTexture after initial capture on MT6789/T606 SoCs. Direct connection fixes this.
 * KEY_REPEAT_PREVIOUS_FRAME_AFTER handles static screen frame repetition.
 *
 * Thermal throttling disabled - in direct mode, encode FPS depends on screen
 * update frequency. Low FPS on static screens is normal, not thermal.
 */
class H264Encoder(
    private val ctx: Context,
    private val projection: MediaProjection,
    initialSender: VideoSender
) {
    companion object {
        private const val TAG = "MirageH264"
        private const val MIME_TYPE = MediaFormat.MIMETYPE_VIDEO_AVC
        private const val MIN_FPS = 10
        private const val MAX_FPS = 60
        private const val BASE_BITRATE = 4_000_000
        private const val BASE_FPS = 30
        private const val LOG_INTERVAL_MS = 5000L
        private const val SPS_PPS_RESEND_INTERVAL_MS = 30000L
        private const val I_FRAME_INTERVAL = 1
    }

    private var codec: MediaCodec? = null
    private var encoderInputSurface: Surface? = null
    private var virtualDisplay: VirtualDisplay? = null
    @Volatile private var running = false
    private var encoderThread: Thread? = null
    private var sendThread: Thread? = null

    private val senderRef = AtomicReference<VideoSender>(initialSender)
    private val secondarySenders = CopyOnWriteArrayList<VideoSender>()

    private val targetFps = AtomicInteger(60)
    @Volatile private var lastSpsPpsSendTimeMs = 0L

    private val packetizer = RtpH264Packetizer(
        ssrc = 0x13572468,
        payloadType = 96,
        mtuPayload = 1200
    )

    private val sendQueue = ArrayBlockingQueue<List<ByteArray>>(32)

    private val projectionCallback = object : MediaProjection.Callback() {
        override fun onStop() {
            Log.i(TAG, "MediaProjection stopped by system")
            stop()
        }
    }

    fun start() {
        val wm = ctx.getSystemService(Context.WINDOW_SERVICE) as android.view.WindowManager
        val width: Int
        val height: Int
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val bounds = wm.currentWindowMetrics.bounds
            width = bounds.width()
            height = bounds.height()
        } else {
            val size = android.graphics.Point()
            @Suppress("DEPRECATION")
            wm.defaultDisplay.getRealSize(size)
            width = size.x
            height = size.y
        }
        val metrics: DisplayMetrics = ctx.resources.displayMetrics
        val dpi = metrics.densityDpi
        val fps = targetFps.get()
        val bitrate = BASE_BITRATE * fps / BASE_FPS

        Log.i(TAG, "Starting encoder v5d-direct: ${width}x${height} @ ${fps}fps, ${bitrate/1000}kbps VBR")

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            projection.registerCallback(projectionCallback, Handler(Looper.getMainLooper()))
        }

        val format = MediaFormat.createVideoFormat(MIME_TYPE, width, height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT,
                MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, I_FRAME_INTERVAL)
            setInteger(MediaFormat.KEY_BITRATE_MODE,
                MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                setInteger(MediaFormat.KEY_PRIORITY, 0)
            }
            setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, 100_000L)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            }
        }

        codec = MediaCodec.createEncoderByType(MIME_TYPE).apply {
            configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        }

        encoderInputSurface = codec!!.createInputSurface()
        codec!!.start()

        // DIRECT MODE: VirtualDisplay -> MediaCodec input surface
        // DO NOT use SurfaceRepeater - it causes frame freeze on MT6789/T606
        virtualDisplay = projection.createVirtualDisplay(
            "mirage_capture", width, height, dpi,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC,
            encoderInputSurface, null, null
        )
        Log.i(TAG, "DIRECT mode active: VirtualDisplay -> MediaCodec (NO SurfaceRepeater)")

        running = true
        sendThread = Thread({ sendLoop() }, "H264Send").also { it.start() }
        encoderThread = Thread({ encoderLoop() }, "H264Encoder").also { it.start() }

        Log.i(TAG, "Encoder v5d-direct started: ${fps}fps, ${bitrate/1000}kbps VBR, IFrame=${I_FRAME_INTERVAL}s")
    }

    fun stop() {
        Log.i(TAG, "Stopping encoder")
        running = false
        encoderThread?.join(1000)
        sendThread?.join(1000)
        virtualDisplay?.release()
        encoderInputSurface?.release()
        encoderInputSurface = null
        for (sec in secondarySenders) {
            try { sec.close() } catch (_: Exception) {}
        }
        secondarySenders.clear()
        try { codec?.stop() } catch (e: Exception) { Log.w(TAG, "Error stopping codec", e) }
        codec?.release()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            try { projection.unregisterCallback(projectionCallback) } catch (_: Exception) {}
        }
        Log.i(TAG, "Encoder stopped")
    }

    fun updateTargetFps(newFps: Int) {
        val fps = newFps.coerceIn(MIN_FPS, MAX_FPS)
        val oldFps = targetFps.getAndSet(fps)
        if (fps == oldFps) return
        val newBitrate = BASE_BITRATE * fps / BASE_FPS
        try {
            codec?.setParameters(Bundle().apply {
                putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, newBitrate)
            })
            Log.i(TAG, "FPS updated: $oldFps -> $fps (${newBitrate/1000}kbps)")
        } catch (e: Exception) { Log.w(TAG, "Failed to update bitrate", e) }
    }

    fun getTargetFps(): Int = targetFps.get()

    fun requestIdr() {
        try {
            codec?.setParameters(Bundle().apply {
                putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
            })
        } catch (_: Exception) {}
    }

    fun switchSender(newSender: VideoSender) {
        val old = senderRef.getAndSet(newSender)
        try { old.close() } catch (_: Exception) {}
        Log.i(TAG, "Sender switched to ${newSender.javaClass.simpleName}")
    }

    fun addSecondarySender(sender: VideoSender) {
        secondarySenders.add(sender)
        Log.i(TAG, "Secondary sender added: ${sender.javaClass.simpleName}")
    }

    fun removeSecondarySender(sender: VideoSender) {
        secondarySenders.remove(sender)
        try { sender.close() } catch (_: Exception) {}
        Log.i(TAG, "Secondary sender removed: ${sender.javaClass.simpleName}")
    }

    private fun sendLoop() {
        Log.i(TAG, "Send loop started (batch mode + secondary)")
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
        while (running) {
            try {
                val packets = sendQueue.poll(50, java.util.concurrent.TimeUnit.MILLISECONDS) ?: continue
                val sender = senderRef.get()
                for (pkt in packets) { sender.send(pkt) }
                sender.flush() // FIX-3: interface経由、instanceof不要
                for (sec in secondarySenders) {
                    try { for (pkt in packets) { sec.send(pkt) }; sec.flush() }
                    catch (e: Exception) { Log.w(TAG, "Secondary sender error: ${e.message}") }
                }
            } catch (e: InterruptedException) { break }
            catch (e: Exception) { Log.w(TAG, "Send error", e) }
        }
        Log.i(TAG, "Send loop ended")
    }

    private fun encoderLoop() {
        val bufferInfo = MediaCodec.BufferInfo()
        var frameCount = 0L
        var lastLogTime = System.currentTimeMillis()
        var lastLogFrameCount = 0L

        while (running) {
            val outputIndex = codec!!.dequeueOutputBuffer(bufferInfo, 10_000)

            if (outputIndex >= 0) {
                val outputBuffer = codec!!.getOutputBuffer(outputIndex)
                if (outputBuffer != null && bufferInfo.size > 0) {
                    val data = ByteArray(bufferInfo.size)
                    outputBuffer.position(bufferInfo.offset)
                    outputBuffer.limit(bufferInfo.offset + bufferInfo.size)
                    outputBuffer.get(data)

                    packetizer.setTimestamp90k(bufferInfo.presentationTimeUs * 90 / 1000)
                    val nals = AnnexBSplitter.splitToNals(data)
                    val nowMs = System.currentTimeMillis()
                    val shouldResendSpsPps = (nowMs - lastSpsPpsSendTimeMs >= SPS_PPS_RESEND_INTERVAL_MS)

                    val batch = mutableListOf<ByteArray>()
                    for ((idx, nal) in nals.withIndex()) {
                        packetizer.cacheParameterSets(nal)
                        if (packetizer.isIdr(nal) || (idx == 0 && shouldResendSpsPps)) {
                            packetizer.sendCachedSpsPps { batch.add(it) }
                            if (shouldResendSpsPps) lastSpsPpsSendTimeMs = nowMs
                        }
                        packetizer.packetizeNalPayload(nal, marker = (idx == nals.size - 1)) { batch.add(it) }
                    }

                    if (!sendQueue.offer(batch)) { sendQueue.poll(); sendQueue.offer(batch) }
                    frameCount++

                    if (nowMs - lastLogTime >= LOG_INTERVAL_MS) {
                        val elapsed = (nowMs - lastLogTime) / 1000.0
                        val encodeFps = (frameCount - lastLogFrameCount) / elapsed
                        Log.i(TAG, "Encode: %.1f fps, frame=%d, size=%d, key=%b, q=%d, sec=%d".format(
                            encodeFps, frameCount, data.size,
                            (bufferInfo.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0,
                            sendQueue.size, secondarySenders.size))
                        lastLogFrameCount = frameCount
                        lastLogTime = nowMs
                    }
                }
                codec!!.releaseOutputBuffer(outputIndex, false)
            } else if (outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                Log.i(TAG, "Output format: ${codec!!.outputFormat}")
            }
        }
        Log.i(TAG, "Encoder loop ended, total frames: $frameCount")
    }
}
