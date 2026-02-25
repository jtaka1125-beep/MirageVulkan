package com.mirage.capture.capture

import android.graphics.PixelFormat
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.ImageReader
import android.media.projection.MediaProjection
import android.util.Log
import com.mirage.capture.ai.AiFrameProducer
import com.mirage.capture.ai.AiJpegSender

/**
 * AI stream: MediaProjection -> VirtualDisplay -> ImageReader(RGBA) -> JPEG -> TCP
 * Completely separate from GUI/H.264 ports.
 */
class AiStream(private val projection: MediaProjection) {
    private val TAG = "AiStream"

    private var vd: VirtualDisplay? = null
    private var reader: ImageReader? = null
    private var producer: AiFrameProducer? = null
    private var sender: AiJpegSender? = null

    fun start(host: String, port: Int, width: Int, height: Int, dpi: Int, fps: Int, quality: Int) {
        stop()
        if (host.isBlank() || port <= 0 || width <= 0 || height <= 0) {
            Log.w(TAG, "Invalid AI stream params")
            return
        }

        val s = AiJpegSender(host, port)
        sender = s

        val r = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 2)
        reader = r

        vd = projection.createVirtualDisplay(
            "mirage_ai",
            width,
            height,
            dpi,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC,
            r.surface,
            null,
            null
        )

        producer = AiFrameProducer(r, fps.coerceIn(1, 30), quality) { jpeg, w, h, tsUs ->
            sender?.send(jpeg, w, h, tsUs)
        }
        producer?.start()
        Log.i(TAG, "AI stream started ${width}x${height} -> $host:$port fps=$fps q=$quality")
    }

    fun stop() {
        try { producer?.stop() } catch (_: Exception) {}
        producer = null
        try { vd?.release() } catch (_: Exception) {}
        vd = null
        try { reader?.close() } catch (_: Exception) {}
        reader = null
        try { sender?.close() } catch (_: Exception) {}
        sender = null
    }
}
