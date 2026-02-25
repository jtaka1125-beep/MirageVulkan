package com.mirage.capture.ai

import android.graphics.Bitmap
import android.graphics.PixelFormat
import android.media.Image
import android.media.ImageReader
import android.os.SystemClock
import android.util.Log
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer

/**
 * Converts ImageReader RGBA_8888 frames to JPEG.
 * NOTE: This is separate from GUI/H.264 stream. Target FPS is low (e.g. 5-15).
 */
class AiFrameProducer(
    private val reader: ImageReader,
    private val targetFps: Int,
    private val jpegQuality: Int,
    private val onJpeg: (ByteArray, Int, Int, Long) -> Unit,
) {
    private val TAG = "AiFrameProducer"

    @Volatile private var running = false
    private var thread: Thread? = null

    fun start() {
        if (running) return
        running = true
        thread = Thread({ loop() }, "ai-frame-producer")
        thread!!.start()
        Log.i(TAG, "Started targetFps=$targetFps q=$jpegQuality")
    }

    fun stop() {
        running = false
        try { thread?.join(1200) } catch (_: Exception) {}
        thread = null
    }

    private fun loop() {
        val intervalMs = (1000.0 / targetFps.toDouble()).toLong().coerceAtLeast(1L)
        var next = SystemClock.uptimeMillis()
        while (running) {
            val now = SystemClock.uptimeMillis()
            if (now < next) {
                SystemClock.sleep((next - now).coerceAtMost(10))
                continue
            }
            next += intervalMs

            var img: Image? = null
            try {
                img = reader.acquireLatestImage()
                if (img == null) continue
                if (img.format != PixelFormat.RGBA_8888) {
                    continue
                }
                val w = img.width
                val h = img.height
                val plane = img.planes[0]
                val buf: ByteBuffer = plane.buffer
                val rowStride = plane.rowStride
                val pixelStride = plane.pixelStride

                // Create Bitmap and copy respecting stride
                val bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
                val pixels = IntArray(w * h)

                var offset = 0
                val row = ByteArray(rowStride)
                for (y in 0 until h) {
                    buf.position(y * rowStride)
                    buf.get(row, 0, rowStride)
                    var xOff = 0
                    for (x in 0 until w) {
                        val r = row[xOff] .toInt() and 0xff
                        val g = row[xOff + 1].toInt() and 0xff
                        val b = row[xOff + 2].toInt() and 0xff
                        val a = row[xOff + 3].toInt() and 0xff
                        pixels[offset++] = (a shl 24) or (r shl 16) or (g shl 8) or b
                        xOff += pixelStride
                    }
                }
                bmp.setPixels(pixels, 0, w, 0, 0, w, h)

                val baos = ByteArrayOutputStream(w * h / 4)
                bmp.compress(Bitmap.CompressFormat.JPEG, jpegQuality.coerceIn(30, 95), baos)
                val jpeg = baos.toByteArray()
                val tsUs = System.nanoTime() / 1000L
                onJpeg(jpeg, w, h, tsUs)
            } catch (_: Exception) {
                // ignore
            } finally {
                try { img?.close() } catch (_: Exception) {}
            }
        }
    }
}
