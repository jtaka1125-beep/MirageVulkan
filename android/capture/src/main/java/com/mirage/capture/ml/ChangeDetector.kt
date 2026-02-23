package com.mirage.capture.ml

import android.graphics.Bitmap
import android.util.Log

class ChangeDetector {
    companion object {
        private const val TAG = "MirageML"
        private const val DETECT_SIZE = 128
        private const val CHANGE_THRESHOLD = 0.05f
    }

    private var prevFrame: ByteArray? = null

    fun detect(bitmap: Bitmap): Boolean {
        val scaled = Bitmap.createScaledBitmap(bitmap, DETECT_SIZE, DETECT_SIZE, true)
        val pixels = IntArray(DETECT_SIZE * DETECT_SIZE)
        scaled.getPixels(pixels, 0, DETECT_SIZE, 0, 0, DETECT_SIZE, DETECT_SIZE)
        scaled.recycle()

        val current = ByteArray(pixels.size)
        for (i in pixels.indices) {
            val p = pixels[i]
            current[i] = (((p shr 16 and 0xFF) * 77 + (p shr 8 and 0xFF) * 150 + (p and 0xFF) * 29) shr 8).toByte()
        }

        val prev = prevFrame
        prevFrame = current
        if (prev == null) return true

        var diffCount = 0
        for (i in current.indices) {
            if (Math.abs((current[i].toInt() and 0xFF) - (prev[i].toInt() and 0xFF)) > 30) diffCount++
        }
        return diffCount.toFloat() / current.size > CHANGE_THRESHOLD
    }

    fun release() { prevFrame = null }
}
