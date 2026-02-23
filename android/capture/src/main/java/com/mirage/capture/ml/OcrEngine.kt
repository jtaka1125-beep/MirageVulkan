package com.mirage.capture.ml

import android.graphics.Bitmap
import android.util.Log
import com.google.mlkit.vision.common.InputImage
import com.google.mlkit.vision.text.TextRecognition
import com.google.mlkit.vision.text.japanese.JapaneseTextRecognizerOptions
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlin.coroutines.resume

class OcrEngine {
    companion object { private const val TAG = "MirageML" }

    private val recognizer = TextRecognition.getClient(JapaneseTextRecognizerOptions.Builder().build())

    suspend fun recognize(bitmap: Bitmap): List<OcrText> = suspendCancellableCoroutine { cont ->
        recognizer.process(InputImage.fromBitmap(bitmap, 0))
            .addOnSuccessListener { result ->
                val imgW = bitmap.width.toFloat(); val imgH = bitmap.height.toFloat()
                val texts = result.textBlocks.mapNotNull { block ->
                    block.boundingBox?.let { box ->
                        OcrText(block.text, box.left/imgW, box.top/imgH, box.width()/imgW, box.height()/imgH,
                            block.lines.firstOrNull()?.confidence ?: 0f)
                    }
                }
                Log.i(TAG, "OCR: ${texts.size} blocks, ${result.text.length} chars")
                cont.resume(texts)
            }
            .addOnFailureListener { e -> Log.e(TAG, "OCR failed: ${e.message}"); cont.resume(emptyList()) }
    }

    fun release() { recognizer.close() }
}
