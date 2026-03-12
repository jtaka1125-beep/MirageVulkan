package com.mirage.capture.capture

import android.content.Context
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
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
    initialSender: VideoSender,
    initialFps: Int = 60,
    initialMaxSize: Int = 0
) {
    companion object {
        private const val TAG = "MirageH265"

        private fun isRepeaterSafe(): Boolean {
            val hw = android.os.Build.HARDWARE?.lowercase() ?: ""
            val model = android.os.Build.MODEL?.lowercase() ?: ""
            val soc = try { android.os.Build.SOC_MODEL?.lowercase() ?: "" } catch (_: Throwable) { "" }
            // Allow override via reflection (adb shell setprop mirage.repeater.force 1)
            val forceEnable = try {
                val cls = Class.forName("android.os.SystemProperties")
                val method = cls.getMethod("get", String::class.java, String::class.java)
                method.invoke(null, "mirage.repeater.force", "0") == "1"
            } catch (_: Throwable) { false }
            if (forceEnable) return true
            // Conservative blacklist
            if (model.contains("t606")) return false
            if (soc.contains("mt6789")) return false
            // X1 (npad): SurfaceRepeater causes timestamp backward -> frame drop
            if (model.contains("npad")) return false
            return true
        }

        private fun mimeType() = MediaFormat.MIMETYPE_VIDEO_HEVC

        // Scale down if long side > maxSz. 0=no limit. 2-aligned.
        fun applyMaxSize(w: Int, h: Int, maxSz: Int): Pair<Int, Int> {
            if (maxSz <= 0 || maxOf(w, h) <= maxSz) return Pair(w, h)
            val sc = maxSz.toDouble() / maxOf(w, h).toDouble()
            val nw = ((w * sc).toInt() / 2) * 2
            val nh = ((h * sc).toInt() / 2) * 2
            return Pair(nw.coerceAtLeast(2), nh.coerceAtLeast(2))
        }

                private const val MIN_FPS = 10
        private const val MAX_FPS = 60
        private const val BASE_BITRATE = 5_000_000
        private const val BASE_FPS = 30
        private const val BITRATE_CAP = 10_000_000
        private const val LOG_INTERVAL_MS = 5000L
        private const val SPS_PPS_RESEND_INTERVAL_MS = 5000L
        private const val I_FRAME_INTERVAL = 2   // IDR every 2s: faster recovery from frame errors
    }

    private var codec: MediaCodec? = null
    private var encoderInputSurface: Surface? = null
    private var virtualDisplay: VirtualDisplay? = null
    private var repeater: SurfaceRepeater? = null
    @Volatile private var running = false
    private var encoderThread: Thread? = null
    private var sendThread: Thread? = null

    private val senderRef = AtomicReference<VideoSender>(initialSender)
    private val secondarySenders = CopyOnWriteArrayList<VideoSender>()

    private val targetFps = AtomicInteger(initialFps.coerceIn(MIN_FPS, MAX_FPS))
    @Volatile private var lastSpsPpsSendTimeMs = 0L
    @Volatile private var idrPending = false  // Drop P-frames until first IDR after new client connect

    private val packetizer = RtpH264Packetizer(
        ssrc = 0x13572468,
        payloadType = 96,
        // HEVC over TCP: use large MTU to avoid FU-A which corrupts HEVC NAL headers
        mtuPayload = 1200,
        useHevc = true  // Always HEVC

    )

    private val sendQueue = ArrayBlockingQueue<List<ByteArray>>(16)
    private val maxSizeLong = java.util.concurrent.atomic.AtomicInteger(initialMaxSize)
    fun setMaxSize(s: Int) { maxSizeLong.set(s) }

    private val projectionCallback = object : MediaProjection.Callback() {
        override fun onStop() {
            Log.i(TAG, "MediaProjection stopped by system")
            stop()
        }
    }

    /** Returns the physical screen size to use for VirtualDisplay creation.
     *  On N-one Npad X1 (and similar npad devices), maximumWindowMetrics
     *  returns the scaled logical size (1080x1920) instead of the physical
     *  panel size (1200x2000). We force the correct value here.
     */
    private fun physicalScreenSize(): Pair<Int, Int> {
        val model = android.os.Build.MODEL?.lowercase() ?: ""
        if (model.contains("npad")) {
            Log.i(TAG, "physicalScreenSize: npad override -> 1200x2000")
            return Pair(1200, 2000)
        }
        val wm = ctx.getSystemService(Context.WINDOW_SERVICE) as android.view.WindowManager
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val bounds = wm.maximumWindowMetrics.bounds
            Pair(bounds.width(), bounds.height())
        } else {
            val size = android.graphics.Point()
            @Suppress("DEPRECATION")
            wm.defaultDisplay.getRealSize(size)
            Pair(size.x, size.y)
        }
    }


    private fun getIntOrNull(format: MediaFormat, key: String): Int? =
        if (format.containsKey(key)) format.getInteger(key) else null

    private fun dumpCodecCapabilities(reqW: Int, reqH: Int, altW: Int, altH: Int, fps: Int) {
        val mimeList = listOf(
            MediaFormat.MIMETYPE_VIDEO_HEVC,
            MediaFormat.MIMETYPE_VIDEO_VP9,
        )
        for (mime in mimeList) {
            try {
                val infos = MediaCodecList(MediaCodecList.ALL_CODECS).codecInfos.filter { info ->
                    info.isEncoder && info.supportedTypes.any { it.equals(mime, ignoreCase = true) }
                }
                Log.i(TAG, "CODEC survey start: mime=${mime}, req=${reqW}x${reqH}@${fps}, alt=${altW}x${altH}@${fps}, codecs=${infos.size}")
                for (info in infos) {
                    try {
                        val caps = info.getCapabilitiesForType(mime)
                        val vc = caps.videoCapabilities
                        val heightsForReqW = try { vc.getSupportedHeightsFor(reqW).toString() } catch (_: Throwable) { "n/a" }
                        val widthsForReqH = try { vc.getSupportedWidthsFor(reqH).toString() } catch (_: Throwable) { "n/a" }
                        val heightsForAltW = try { vc.getSupportedHeightsFor(altW).toString() } catch (_: Throwable) { "n/a" }
                        val widthsForAltH = try { vc.getSupportedWidthsFor(altH).toString() } catch (_: Throwable) { "n/a" }
                        val fpsForReq = try { vc.getSupportedFrameRatesFor(reqW, reqH).toString() } catch (_: Throwable) { "n/a" }
                        val fpsForAlt = try { vc.getSupportedFrameRatesFor(altW, altH).toString() } catch (_: Throwable) { "n/a" }
                        Log.i(
                            TAG,
                            "CODEC mime=${mime} codec=${info.name} hw=${info.isHardwareAccelerated} sw=${info.isSoftwareOnly} vendor=${info.isVendor} " +
                                "widths=${vc.supportedWidths} heights=${vc.supportedHeights} align=${vc.widthAlignment}x${vc.heightAlignment} " +
                                "reqOK=${vc.isSizeSupported(reqW, reqH)} altOK=${vc.isSizeSupported(altW, altH)} " +
                                "heightsForReqW=${heightsForReqW} widthsForReqH=${widthsForReqH} heightsForAltW=${heightsForAltW} widthsForAltH=${widthsForAltH} " +
                                "fpsForReq=${fpsForReq} fpsForAlt=${fpsForAlt}"
                        )
                    } catch (e: Throwable) {
                        Log.w(TAG, "CODEC capability dump failed: mime=${mime}, codec=${info.name}", e)
                    }
                }
                Log.i(TAG, "CODEC survey end: mime=${mime}")
            } catch (e: Throwable) {
                Log.w(TAG, "CODEC survey failed: mime=${mime}", e)
            }
        }
    }

    fun start() {
        val wm = ctx.getSystemService(Context.WINDOW_SERVICE) as android.view.WindowManager
        val (rawWidth, rawHeight) = physicalScreenSize()
        val rawCW = minOf(rawWidth, rawHeight)
        val rawCH = maxOf(rawWidth, rawHeight)
        val (captureWidth, captureHeight) = applyMaxSize(rawCW, rawCH, maxSizeLong.get())
        val metrics: DisplayMetrics = ctx.resources.displayMetrics
        val dpi = metrics.densityDpi
        val fps = targetFps.get()
        val bitrate = (BASE_BITRATE * fps / BASE_FPS).coerceAtMost(BITRATE_CAP)

        val windowBounds = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) wm.maximumWindowMetrics.bounds else null
        Log.i(
            TAG,
            "Starting encoder v6-probe: raw=${rawWidth}x${rawHeight}, capture=${captureWidth}x${captureHeight}, " +
                "window=${windowBounds?.width()}x${windowBounds?.height()}, dpi=${dpi}, fps=${fps}, bitrate=${bitrate/1000}kbps"
        )
        dumpCodecCapabilities(captureWidth, captureHeight, rawWidth, rawHeight, fps)

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            projection.registerCallback(projectionCallback, Handler(Looper.getMainLooper()))
        }

        val effectiveMime = mimeType()  // Always HEVC
        Log.i(TAG, "Encoder mime: $effectiveMime")
        val format = MediaFormat.createVideoFormat(effectiveMime, captureWidth, captureHeight).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT,
                MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, I_FRAME_INTERVAL)
            setInteger(MediaFormat.KEY_BITRATE_MODE,
                MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                setInteger(MediaFormat.KEY_PRIORITY, 0)
            }            // Use encoder-side repeat to sustain target fps even when VirtualDisplay supply is capped (often ~30fps).
            val repeatUs = (1_000_000L / fps.toLong()).coerceAtLeast(10_000L)
            setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, repeatUs)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            }
            // HEVC: prepend VPS/SPS/PPS before every IDR so PC can detect stream start
            if (effectiveMime == MediaFormat.MIMETYPE_VIDEO_HEVC) {
                setInteger(MediaFormat.KEY_PREPEND_HEADER_TO_SYNC_FRAMES, 1)
            }
        }

        Log.i(TAG, "Requested format: ${format}")
        Log.i(TAG, "RepeatPrevFrameAfter set for fps=$fps")

        codec = try {
            val swName = "OMX.google.hevc.encoder"
            Log.i(TAG, "Using SW HEVC encoder: $swName for ${captureWidth}x${captureHeight}")
            MediaCodec.createByCodecName(swName)
        } catch (e: Exception) {
            Log.w(TAG, "SW HEVC encoder unavailable ($e), falling back to HW HEVC")
            MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_HEVC)
        }.apply { configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE) }
        encoderInputSurface = codec!!.createInputSurface()
        codec!!.start()

        // MODE SELECT:
        // Many devices cap VirtualDisplay supply to ~30fps. When targeting 50-60fps,
        // use SurfaceRepeater to swap encoder surface at target fps using the latest frame.
        if (fps >= 50 && isRepeaterSafe()) {
            repeater = SurfaceRepeater(captureWidth, captureHeight, fps, encoderInputSurface!!) { srcSurface ->
                virtualDisplay = projection.createVirtualDisplay(
                    "mirage_capture", captureWidth, captureHeight, dpi,
                    DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC,
                    srcSurface, null, null
                )
            }
            repeater?.start()
            Log.i(TAG, "REPEATER mode active: VirtualDisplay(${captureWidth}x${captureHeight}) -> SurfaceTexture -> MediaCodec @$fps")
        } else {
            virtualDisplay = projection.createVirtualDisplay(
                "mirage_capture", captureWidth, captureHeight, dpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC,
                encoderInputSurface, null, null
            )
            Log.i(TAG, "DIRECT mode active: VirtualDisplay(${captureWidth}x${captureHeight}) -> MediaCodec")
        }

        running = true
        sendThread = Thread({ sendLoop() }, "H264Send").also { it.start() }
        encoderThread = Thread({ encoderLoop() }, "H264Encoder").also { it.start() }

        Log.i(TAG, "Encoder v6-probe started: capture=${captureWidth}x${captureHeight}, ${fps}fps, ${bitrate/1000}kbps VBR, IFrame=${I_FRAME_INTERVAL}s")
    }

    fun stop() {
        Log.i(TAG, "Stopping encoder")
        running = false
        encoderThread?.join(1000)
        sendThread?.join(1000)
        try { repeater?.stop() } catch (_: Exception) {}
        repeater = null
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
        val newBitrate = (BASE_BITRATE * fps / BASE_FPS).coerceAtMost(BITRATE_CAP)
        try {
            codec?.setParameters(Bundle().apply {
                putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, newBitrate)
            })
            Log.i(TAG, "FPS updated: $oldFps -> $fps (${newBitrate/1000}kbps)")
        } catch (e: Exception) { Log.w(TAG, "Failed to update bitrate", e) }
    }

    fun getTargetFps(): Int = targetFps.get()

    fun requestIdr() {
        idrPending = true
        lastSpsPpsSendTimeMs = 0L  // Force VPS/SPS/PPS resend with first IDR
        sendQueue.clear()  // Discard stale frames
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
                val packets = sendQueue.poll(10, java.util.concurrent.TimeUnit.MILLISECONDS) ?: continue
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
        var lastSentPtsUs = 0L
        var lastLogTime = System.currentTimeMillis()
        var lastLogFrameCount = 0L

        while (running) {
            val outputIndex = codec!!.dequeueOutputBuffer(bufferInfo, 5_000)

            if (outputIndex >= 0) {
                val outputBuffer = codec!!.getOutputBuffer(outputIndex)
                if (outputBuffer != null && bufferInfo.size > 0) {
                    // Drop non-IDR frames until first IDR after new client connect
                    val isKeyFrame = (bufferInfo.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0
                    if (idrPending && !isKeyFrame) {
                        codec!!.releaseOutputBuffer(outputIndex, false)
                        continue
                    }
                    if (idrPending && isKeyFrame) {
                        idrPending = false
                        sendQueue.clear()  // Clear any P-frames that snuck in
                        Log.i(TAG, "IDR arrived, resuming stream for new client")
                    }

                    val data = ByteArray(bufferInfo.size)
                    outputBuffer.position(bufferInfo.offset)
                    outputBuffer.limit(bufferInfo.offset + bufferInfo.size)
                    outputBuffer.get(data)

                    // Throttle actual send rate to target FPS (important on devices where VDS runs > target).
                    val tfps = targetFps.get()
                    if (tfps <= 35) {
                        val minIntervalUs = 1_000_000L / tfps.toLong()
                        val pts = bufferInfo.presentationTimeUs
                        val isKey = (bufferInfo.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0
                        if (!isKey && lastSentPtsUs != 0L && (pts - lastSentPtsUs) < minIntervalUs) {
                            codec!!.releaseOutputBuffer(outputIndex, false)
                            continue
                        }
                        lastSentPtsUs = pts
                    }

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
                codec!!.outputFormat.also { of -> Log.i(TAG, "Output format: ${of}, coded=${getIntOrNull(of, MediaFormat.KEY_WIDTH)}x${getIntOrNull(of, MediaFormat.KEY_HEIGHT)}, crop=${getIntOrNull(of, "crop-left")},${getIntOrNull(of, "crop-top")}-${getIntOrNull(of, "crop-right")},${getIntOrNull(of, "crop-bottom")}, stride=${getIntOrNull(of, MediaFormat.KEY_STRIDE)}, sliceHeight=${getIntOrNull(of, MediaFormat.KEY_SLICE_HEIGHT)}, color=${getIntOrNull(of, MediaFormat.KEY_COLOR_FORMAT)}") }
            }
        }
        Log.i(TAG, "Encoder loop ended, total frames: $frameCount")
    }
}




