package com.mirage.capture.ml

import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.util.Log
import kotlinx.coroutines.*

class ScreenAnalyzer(private val context: Context) {
    companion object {
        private const val TAG = "MirageML"
        const val ACTION_ANALYSIS_RESULT = "com.mirage.capture.ANALYSIS_RESULT"
    }

    private val changeDetector = ChangeDetector()
    private var screenClassifier: ScreenClassifier? = null
    private var uiDetector: UiDetector? = null
    private var ocrEngine: OcrEngine? = null
    private val scope = CoroutineScope(Dispatchers.Default + SupervisorJob())
    private var analysisJob: Job? = null

    @Volatile private var pendingFrame: Bitmap? = null
    @Volatile private var running = false
    @Volatile var enableOcr = true
    @Volatile var enableDetection = true
    @Volatile var enableClassification = true

    var onResult: ((AnalysisResult) -> Unit)? = null
    @Volatile var totalAnalyses = 0L; private set
    @Volatile var totalChanges = 0L; private set
    @Volatile var avgPipelineMs = 0f; private set

    fun start(intervalMs: Long = 100) {
        if (running) return; running = true
        Log.i(TAG, "ScreenAnalyzer starting (interval=${intervalMs}ms)")
        analysisJob = scope.launch {
            while (isActive && running) {
                val frame = pendingFrame
                if (frame != null && !frame.isRecycled) {
                    pendingFrame = null
                    try { onResult?.invoke(analyzeFrame(frame)) } catch (e: Exception) { Log.e(TAG, "Analysis error: ${e.message}") }
                }
                delay(intervalMs)
            }
        }
    }

    fun stop() { running = false; analysisJob?.cancel(); analysisJob = null
        Log.i(TAG, "ScreenAnalyzer stopped (total=$totalAnalyses changes=$totalChanges avgMs=${"%.1f".format(avgPipelineMs)})") }

    fun submitFrame(bitmap: Bitmap) {
        if (!running) return
        pendingFrame = bitmap.copy(Bitmap.Config.ARGB_8888, false)
    }

    private suspend fun analyzeFrame(frame: Bitmap): AnalysisResult {
        val startTime = System.currentTimeMillis(); totalAnalyses++
        val tier1Start = System.currentTimeMillis()
        val changed = changeDetector.detect(frame)
        val tier1Ms = System.currentTimeMillis() - tier1Start

        if (!changed) { frame.recycle(); return AnalysisResult(screenChanged=false, tier1Ms=tier1Ms, pipelineMs=System.currentTimeMillis()-startTime) }

        totalChanges++
        val tier2Start = System.currentTimeMillis()
        ensureTier2Initialized()

        val classifyResult = if (enableClassification) screenClassifier?.classify(frame) else null
        val detections = if (enableDetection) uiDetector?.detect(frame) ?: emptyList() else emptyList()
        val ocrTexts = if (enableOcr) ocrEngine?.recognize(frame) ?: emptyList() else emptyList()

        val tier2Ms = System.currentTimeMillis() - tier2Start
        val pipelineMs = System.currentTimeMillis() - startTime
        frame.recycle()
        avgPipelineMs = avgPipelineMs * 0.9f + pipelineMs * 0.1f

        val result = AnalysisResult(screenChanged=true, screenState=classifyResult?.state ?: ScreenState.UNKNOWN,
            confidence=classifyResult?.confidence ?: 0f, detections=detections, ocrTexts=ocrTexts,
            pipelineMs=pipelineMs, tier1Ms=tier1Ms, tier2Ms=tier2Ms)

        Log.i(TAG, "Analysis: state=${result.screenState} det=${detections.size} ocr=${ocrTexts.size} t1=${tier1Ms}ms t2=${tier2Ms}ms total=${pipelineMs}ms")
        broadcastResult(result)
        return result
    }

    private fun ensureTier2Initialized() {
        if (screenClassifier == null && enableClassification) screenClassifier = ScreenClassifier(context)
        if (uiDetector == null && enableDetection) uiDetector = UiDetector(context)
        if (ocrEngine == null && enableOcr) ocrEngine = OcrEngine()
    }

    private fun broadcastResult(result: AnalysisResult) {
        context.sendBroadcast(Intent(ACTION_ANALYSIS_RESULT).apply {
            putExtra("screen_changed", result.screenChanged)
            putExtra("screen_state", result.screenState.name)
            putExtra("confidence", result.confidence)
            putExtra("detection_count", result.detections.size)
            putExtra("ocr_count", result.ocrTexts.size)
            putExtra("pipeline_ms", result.pipelineMs)
            putExtra("ocr_texts", result.ocrTexts.take(5).joinToString("|") { it.text })
            putExtra("detections", result.detections.take(10).joinToString("|") {
                "${it.label}:${"%.2f".format(it.confidence)}:${"%.3f".format(it.x)}:${"%.3f".format(it.y)}:${"%.3f".format(it.width)}:${"%.3f".format(it.height)}" })
        })
    }

    fun release() { stop(); changeDetector.release(); screenClassifier?.release(); uiDetector?.release(); ocrEngine?.release(); scope.cancel()
        Log.i(TAG, "ScreenAnalyzer released") }
}
