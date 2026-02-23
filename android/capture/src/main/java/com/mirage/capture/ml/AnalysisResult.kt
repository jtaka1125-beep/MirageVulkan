package com.mirage.capture.ml

data class AnalysisResult(
    val timestamp: Long = System.currentTimeMillis(),
    val screenChanged: Boolean = false,
    val screenState: ScreenState = ScreenState.UNKNOWN,
    val confidence: Float = 0f,
    val detections: List<Detection> = emptyList(),
    val ocrTexts: List<OcrText> = emptyList(),
    val pipelineMs: Long = 0,
    val tier1Ms: Long = 0,
    val tier2Ms: Long = 0,
)

enum class ScreenState {
    UNKNOWN, HOME, APP, LOADING, ERROR, DIALOG, KEYBOARD, OFF
}

data class Detection(
    val label: String,
    val confidence: Float,
    val x: Float, val y: Float,
    val width: Float, val height: Float,
)

data class OcrText(
    val text: String,
    val x: Float, val y: Float,
    val width: Float, val height: Float,
    val confidence: Float = 0f,
)
