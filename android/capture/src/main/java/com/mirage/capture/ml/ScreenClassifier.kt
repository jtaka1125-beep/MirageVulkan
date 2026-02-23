package com.mirage.capture.ml

import android.content.Context
import android.graphics.Bitmap
import android.util.Log
import org.tensorflow.lite.Interpreter
import org.tensorflow.lite.support.common.FileUtil
import java.nio.ByteBuffer
import java.nio.ByteOrder

class ScreenClassifier(context: Context) {
    companion object {
        private const val TAG = "MirageML"
        private const val MODEL_FILE = "models/screen_classifier.tflite"
        private const val INPUT_SIZE = 224
        private const val NUM_CLASSES = 8
    }

    private var interpreter: Interpreter? = null
    private val inputBuffer = ByteBuffer.allocateDirect(1 * INPUT_SIZE * INPUT_SIZE * 3 * 4).apply { order(ByteOrder.nativeOrder()) }
    private val outputArray = Array(1) { FloatArray(NUM_CLASSES) }

    init {
        try {
            val model = FileUtil.loadMappedFile(context, MODEL_FILE)
            interpreter = Interpreter(model, Interpreter.Options().apply { setNumThreads(2); setUseXNNPACK(true) })
            Log.i(TAG, "ScreenClassifier loaded")
        } catch (e: Exception) {
            Log.w(TAG, "ScreenClassifier model not found, stub mode: ${e.message}")
        }
    }

    data class ClassifyResult(val state: ScreenState, val confidence: Float)

    fun classify(bitmap: Bitmap): ClassifyResult {
        val interp = interpreter ?: return ClassifyResult(ScreenState.UNKNOWN, 0f)
        val scaled = Bitmap.createScaledBitmap(bitmap, INPUT_SIZE, INPUT_SIZE, true)
        inputBuffer.rewind()
        val pixels = IntArray(INPUT_SIZE * INPUT_SIZE)
        scaled.getPixels(pixels, 0, INPUT_SIZE, 0, 0, INPUT_SIZE, INPUT_SIZE)
        scaled.recycle()
        for (pixel in pixels) {
            inputBuffer.putFloat((pixel shr 16 and 0xFF) / 255.0f)
            inputBuffer.putFloat((pixel shr 8 and 0xFF) / 255.0f)
            inputBuffer.putFloat((pixel and 0xFF) / 255.0f)
        }
        interp.run(inputBuffer, outputArray)
        val scores = outputArray[0]
        var maxIdx = 0; var maxScore = scores[0]
        for (i in 1 until scores.size) { if (scores[i] > maxScore) { maxIdx = i; maxScore = scores[i] } }
        return ClassifyResult(ScreenState.entries.getOrElse(maxIdx) { ScreenState.UNKNOWN }, maxScore)
    }

    fun release() { interpreter?.close(); interpreter = null }
}
