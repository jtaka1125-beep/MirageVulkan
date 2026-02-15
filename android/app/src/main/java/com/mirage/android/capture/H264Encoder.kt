package com.mirage.android.capture

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
import android.util.DisplayMetrics
import android.util.Log
import android.view.Surface
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

/**
 * H.264 encoder using MediaCodec with Surface input from VirtualDisplay.
 * Uses SurfaceRepeater (EGL) to maintain consistent frame rate even when
 * the screen is static (works around MediaTek and other SoCs ignoring
 * KEY_REPEAT_PREVIOUS_FRAME_AFTER).
 *
 * Pipeline:
 *   VirtualDisplay → SurfaceRepeater(EGL, 30fps) → MediaCodec → RTP → VideoSender
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
        private const val BASE_BITRATE = 6_000_000  // 6 Mbps at 30fps
        private const val BASE_FPS = 30
        private const val SPS_PPS_RESEND_INTERVAL_MS = 3000L
    }

    private var codec: MediaCodec? = null
    private var encoderInputSurface: Surface? = null
    private var surfaceRepeater: SurfaceRepeater? = null
    private var virtualDisplay: VirtualDisplay? = null
    @Volatile private var running = false
    private var encoderThread: Thread? = null

    private val senderRef = AtomicReference<VideoSender>(initialSender)

    private val targetFps = AtomicInteger(30)
    @Volatile private var frameIntervalNs = 1_000_000_000L / 30
    @Volatile private var lastFrameTimeNs = 0L
    @Volatile private var lastSpsPpsSendTimeMs = 0L

    private val packetizer = RtpH264Packetizer(
        ssrc = 0x13572468,
        payloadType = 96,
        mtuPayload = 1200
    )

    private val projectionCallback = object : MediaProjection.Callback() {
        override fun onStop() {
            Log.i(TAG, "MediaProjection stopped by system")
            stop()
        }
    }

    fun start() {
        val metrics: DisplayMetrics = ctx.resources.displayMetrics
        val width = metrics.widthPixels
        val height = metrics.heightPixels
        val fps = 30
        val bitrate = 6_000_000  // 6 Mbps for full 30fps

        Log.i(TAG, "Starting encoder: ${width}x${height} @ ${fps}fps, ${bitrate}bps")

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            projection.registerCallback(projectionCallback, Handler(Looper.getMainLooper()))
            Log.i(TAG, "Registered MediaProjection callback (Android 14+)")
        }

        val format = MediaFormat.createVideoFormat(MIME_TYPE, width, height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT,
                MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
            // Still set this as a hint - some SoCs do respect it
            setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, 100_000L)
        }

        codec = MediaCodec.createEncoderByType(MIME_TYPE).apply {
            configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        }

        encoderInputSurface = codec!!.createInputSurface()
        codec!!.start()

        // Create SurfaceRepeater: sits between VirtualDisplay and encoder
        // Ensures frames are pushed to encoder at targetFps even on static screens
        surfaceRepeater = SurfaceRepeater(
            outputSurface = encoderInputSurface!!,
            width = width,
            height = height,
            targetFps = fps
        ).also { it.start() }

        val repeaterInput = surfaceRepeater?.inputSurface
        if (repeaterInput == null) {
            Log.e(TAG, "SurfaceRepeater failed to create input surface, falling back to direct")
            // Fallback: VirtualDisplay → encoder directly (old behavior, 2fps on static)
            virtualDisplay = projection.createVirtualDisplay(
                "mirage_capture", width, height, metrics.densityDpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC,
                encoderInputSurface, null, null
            )
        } else {
            // Normal: VirtualDisplay → SurfaceRepeater → encoder
            virtualDisplay = projection.createVirtualDisplay(
                "mirage_capture", width, height, metrics.densityDpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC,
                repeaterInput, null, null
            )
            Log.i(TAG, "SurfaceRepeater active: VirtualDisplay -> EGL(${fps}fps) -> Encoder")
        }

        running = true
        encoderThread = Thread({ encoderLoop(fps) }, "H264Encoder").also { it.start() }

        Log.i(TAG, "Encoder started with SurfaceRepeater")
    }

    fun stop() {
        Log.i(TAG, "Stopping encoder")
        running = false
        encoderThread?.join(1000)

        virtualDisplay?.release()
        surfaceRepeater?.stop()
        surfaceRepeater = null

        encoderInputSurface?.release()
        encoderInputSurface = null

        try {
            codec?.stop()
        } catch (e: Exception) {
            Log.w(TAG, "Error stopping codec", e)
        }
        codec?.release()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            try {
                projection.unregisterCallback(projectionCallback)
            } catch (e: Exception) {
                Log.w(TAG, "Error unregistering callback", e)
            }
        }

        Log.i(TAG, "Encoder stopped")
    }

    fun updateTargetFps(newFps: Int) {
        val fps = newFps.coerceIn(MIN_FPS, MAX_FPS)
        val oldFps = targetFps.getAndSet(fps)

        if (fps == oldFps) return

        frameIntervalNs = 1_000_000_000L / fps

        // Update SurfaceRepeater FPS (reduces GPU draw rate when lowering FPS)
        surfaceRepeater?.updateFps(fps)

        val newBitrate = BASE_BITRATE * fps / BASE_FPS
        try {
            codec?.let { c ->
                val params = Bundle().apply {
                    putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, newBitrate)
                }
                c.setParameters(params)
            }
            Log.i(TAG, "FPS updated: $oldFps -> $fps (bitrate: ${newBitrate / 1000} kbps)")
        } catch (e: Exception) {
            Log.w(TAG, "Failed to update bitrate", e)
        }
    }

    fun getTargetFps(): Int = targetFps.get()

    fun requestIdr() {
        try {
            codec?.let { c ->
                val params = Bundle().apply {
                    putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
                }
                c.setParameters(params)
                Log.i(TAG, "IDR requested")
            }
        } catch (e: Exception) {
            Log.w(TAG, "Failed to request IDR", e)
        }
    }

    fun switchSender(newSender: VideoSender) {
        val old = senderRef.getAndSet(newSender)
        try {
            old.close()
        } catch (e: Exception) {
            Log.w(TAG, "Error closing old sender", e)
        }
        Log.i(TAG, "Sender switched to ${newSender.javaClass.simpleName}")
    }

    private fun shouldDropFrame(): Boolean {
        val now = System.nanoTime()
        if (now - lastFrameTimeNs < frameIntervalNs) {
            return true
        }
        lastFrameTimeNs = now
        return false
    }

    private fun encoderLoop(fps: Int) {
        val bufferInfo = MediaCodec.BufferInfo()
        var frameCount = 0L

        val timestampDelta = 90000 / fps

        while (running) {
            val outputIndex = codec!!.dequeueOutputBuffer(bufferInfo, 10_000)

            if (outputIndex >= 0) {
                val outputBuffer = codec!!.getOutputBuffer(outputIndex)
                if (outputBuffer != null && bufferInfo.size > 0) {
                    val isKeyframe = (bufferInfo.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0

                    if (!isKeyframe && shouldDropFrame()) {
                        codec!!.releaseOutputBuffer(outputIndex, false)
                        continue
                    }

                    val data = ByteArray(bufferInfo.size)
                    outputBuffer.position(bufferInfo.offset)
                    outputBuffer.limit(bufferInfo.offset + bufferInfo.size)
                    outputBuffer.get(data)

                    val ptsUs = bufferInfo.presentationTimeUs
                    val ts90k = (ptsUs * 90 / 1000)
                    packetizer.setTimestamp90k(ts90k)

                    val nals = AnnexBSplitter.splitToNals(data)
                    val currentSender = senderRef.get()

                    val nowMs = System.currentTimeMillis()
                    val shouldResendSpsPps = (nowMs - lastSpsPpsSendTimeMs >= SPS_PPS_RESEND_INTERVAL_MS)

                    for ((idx, nal) in nals.withIndex()) {
                        packetizer.cacheParameterSets(nal)

                        if (packetizer.isIdr(nal) || (idx == 0 && shouldResendSpsPps)) {
                            packetizer.sendCachedSpsPps { rtpPkt ->
                                currentSender.send(rtpPkt)
                            }
                            if (shouldResendSpsPps) lastSpsPpsSendTimeMs = nowMs
                        }

                        val isLastNal = (idx == nals.size - 1)
                        packetizer.packetizeNalPayload(nal, marker = isLastNal) { rtpPkt ->
                            currentSender.send(rtpPkt)
                        }
                    }

                    frameCount++

                    if (frameCount % 30 == 0L) {
                        Log.d(TAG, "Frame $frameCount: size=${data.size}, nals=${nals.size}, keyframe=$isKeyframe, targetFps=${targetFps.get()}")
                    }
                }

                codec!!.releaseOutputBuffer(outputIndex, false)
            } else if (outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                val newFormat = codec!!.outputFormat
                Log.i(TAG, "Output format changed: $newFormat")
            }
        }

        Log.i(TAG, "Encoder loop ended, total frames: $frameCount")
    }
}
