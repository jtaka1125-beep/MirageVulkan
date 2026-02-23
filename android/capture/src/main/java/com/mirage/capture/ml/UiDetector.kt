package com.mirage.capture.ml

import android.content.Context
import android.graphics.Bitmap
import android.util.Log
import org.tensorflow.lite.Interpreter
import org.tensorflow.lite.support.common.FileUtil
import java.nio.ByteBuffer
import java.nio.ByteOrder

class UiDetector(context: Context) {
    companion object {
        private const val TAG = "MirageML"
        private const val MODEL_FILE = "models/ui_detector.tflite"
        private const val INPUT_SIZE = 320
        private const val CONF_THRESHOLD = 0.5f
        private const val MAX_DETECTIONS = 50
    }

    private var interpreter: Interpreter? = null
    private val inputBuffer = ByteBuffer.allocateDirect(1 * INPUT_SIZE * INPUT_SIZE * 3 * 4).apply { order(ByteOrder.nativeOrder()) }
    private val labels = listOf("button", "text_field", "icon", "image", "checkbox", "toggle", "slider", "tab", "menu", "dialog")

    init {
        try {
            val model = FileUtil.loadMappedFile(context, MODEL_FILE)
            interpreter = Interpreter(model, Interpreter.Options().apply { setNumThreads(2); setUseXNNPACK(true) })
            Log.i(TAG, "UiDetector loaded")
        } catch (e: Exception) {
            Log.w(TAG, "UiDetector model not found, stub mode: ${e.message}")
        }
    }

    fun detect(bitmap: Bitmap): List<Detection> {
        val interp = interpreter ?: return emptyList()
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
        val boxes = Array(1) { Array(MAX_DETECTIONS) { FloatArray(4) } }
        val scores = Array(1) { FloatArray(MAX_DETECTIONS) }
        val classes = Array(1) { FloatArray(MAX_DETECTIONS) }
        val numDet = FloatArray(1)
        val outputs = HashMap<Int, Any>().apply { put(0, boxes); put(1, scores); put(2, classes); put(3, numDet) }
        try { interp.runForMultipleInputsOutputs(arrayOf(inputBuffer), outputs) } catch (e: Exception) {
            Log.e(TAG, "UiDetector inference error: ${e.message}"); return emptyList()
        }
        val count = numDet[0].toInt().coerceAtMost(MAX_DETECTIONS)
        return (0 until count).filter { scores[0][it] >= CONF_THRESHOLD }.map { i ->
            val b = boxes[0][i]; Detection(labels.getOrElse(classes[0][i].toInt()) { "unknown" }, scores[0][i],
                b[1].coerceIn(0f,1f), b[0].coerceIn(0f,1f), (b[3]-b[1]).coerceIn(0f,1f), (b[2]-b[0]).coerceIn(0f,1f))
        }.sortedByDescending { it.confidence }
    }

    fun release() { interpreter?.close(); interpreter = null }
}
