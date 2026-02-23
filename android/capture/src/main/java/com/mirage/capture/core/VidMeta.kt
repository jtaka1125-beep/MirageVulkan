package com.mirage.capture.core

data class VidMeta(
    val slot: Int,
    val seq: Long,
    val codec: String,
    val bytes: Int,
    val capMonoNs: Long,
    val capWallMs: Long,
    val keyframe: Boolean,
    val path: String,
    val fpsHint: Int,
) {
    fun toJson(): String {
        // Minimal JSON without external deps. Escaping is conservative for fixed fields.
        return buildString(256) {
            append('{')
            append("\"slot\":").append(slot).append(',')
            append("\"seq\":").append(seq).append(',')
            append("\"codec\":\"").append(codec).append("\",")
            append("\"bytes\":").append(bytes).append(',')
            append("\"cap_mono_ns\":").append(capMonoNs).append(',')
            append("\"cap_wall_ms\":").append(capWallMs).append(',')
            append("\"keyframe\":").append(if (keyframe) "true" else "false").append(',')
            append("\"path\":\"").append(path).append("\",")
            append("\"fps_hint\":").append(fpsHint)
            append('}')
        }
    }
}
