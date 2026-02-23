package com.mirage.capture.audio

import android.media.MediaCodec
import android.media.MediaFormat
import android.util.Log
import java.nio.ByteBuffer

/**
 * OpusEncoder - Encodes PCM audio to Opus format
 *
 * Uses Android MediaCodec for Opus encoding.
 * Falls back to raw PCM if Opus is not available.
 */
class OpusEncoder(
    private val sampleRate: Int,
    private val channels: Int
) {
    companion object {
        private const val TAG = "OpusEncoder"
        private const val MIME_TYPE = MediaFormat.MIMETYPE_AUDIO_OPUS
        private const val BIT_RATE = 64000 // 64 kbps
        private const val FRAME_SIZE_MS = 20
    }

    private var codec: MediaCodec? = null
    private var isInitialized = false
    private var useRawPcm = false
    private var codecErrorCount = 0
    private val maxCodecErrors = 5

    fun init(): Boolean {
        try {
            val format = MediaFormat.createAudioFormat(MIME_TYPE, sampleRate, channels).apply {
                setInteger(MediaFormat.KEY_BIT_RATE, BIT_RATE)
                setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, sampleRate * channels * 2 * FRAME_SIZE_MS / 1000)
            }

            codec = MediaCodec.createEncoderByType(MIME_TYPE)
            codec?.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            codec?.start()

            isInitialized = true
            useRawPcm = false
            codecErrorCount = 0
            Log.i(TAG, "Opus encoder initialized: ${sampleRate}Hz, ${channels}ch, ${BIT_RATE}bps")
            return true
        } catch (e: Exception) {
            Log.w(TAG, "Opus encoder not available, using raw PCM fallback", e)
            useRawPcm = true
            isInitialized = true
            return true
        }
    }

    /**
     * Encode PCM samples to Opus
     * @param pcmData PCM samples (16-bit signed, interleaved stereo)
     * @param sampleCount Number of samples (total, including both channels)
     * @param outputBuffer Buffer to receive encoded data
     * @return Size of encoded data, or -1 on error
     */
    fun encode(pcmData: ShortArray, sampleCount: Int, outputBuffer: ByteArray): Int {
        if (!isInitialized) return -1

        // Fallback: send raw PCM (for devices without Opus support)
        if (useRawPcm) {
            return encodeRawPcm(pcmData, sampleCount, outputBuffer)
        }

        val codec = this.codec ?: return -1

        try {
            // Queue input buffer with timeout
            val inputIndex = codec.dequeueInputBuffer(5000)
            if (inputIndex >= 0) {
                val inputBuffer = codec.getInputBuffer(inputIndex)
                inputBuffer?.clear()

                // Convert short array to byte array (little-endian)
                val byteCount = minOf(sampleCount * 2, inputBuffer?.remaining() ?: 0)
                val byteBuffer = ByteBuffer.allocate(byteCount)
                byteBuffer.order(java.nio.ByteOrder.LITTLE_ENDIAN)
                byteBuffer.asShortBuffer().put(pcmData, 0, byteCount / 2)
                inputBuffer?.put(byteBuffer.array(), 0, byteCount)

                codec.queueInputBuffer(inputIndex, 0, byteCount, System.nanoTime() / 1000, 0)
            } else if (inputIndex == MediaCodec.INFO_TRY_AGAIN_LATER) {
                // No input buffer available, skip this frame
                return 0
            }

            // Get output buffer
            val bufferInfo = MediaCodec.BufferInfo()
            val outputIndex = codec.dequeueOutputBuffer(bufferInfo, 5000)

            when {
                outputIndex >= 0 -> {
                    val outputBufferData = codec.getOutputBuffer(outputIndex)
                    val size = bufferInfo.size

                    if (size > 0 && outputBufferData != null && size <= outputBuffer.size) {
                        outputBufferData.get(outputBuffer, 0, size)
                        codec.releaseOutputBuffer(outputIndex, false)
                        codecErrorCount = 0  // Reset error count on success
                        return size
                    }
                    codec.releaseOutputBuffer(outputIndex, false)
                }
                outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                    val newFormat = codec.outputFormat
                    Log.i(TAG, "Output format changed: $newFormat")
                }
                outputIndex == MediaCodec.INFO_TRY_AGAIN_LATER -> {
                    // No output available yet
                }
            }

            return 0 // No output yet
        } catch (e: MediaCodec.CodecException) {
            Log.e(TAG, "MediaCodec error: ${e.diagnosticInfo}", e)
            codecErrorCount++
            if (codecErrorCount >= maxCodecErrors) {
                Log.w(TAG, "Too many codec errors, switching to raw PCM")
                switchToRawPcm()
            }
            return -1
        } catch (e: IllegalStateException) {
            Log.e(TAG, "Codec in illegal state", e)
            codecErrorCount++
            if (codecErrorCount >= maxCodecErrors) {
                switchToRawPcm()
            }
            return -1
        } catch (e: Exception) {
            Log.e(TAG, "Encode error", e)
            return -1
        }
    }

    private fun switchToRawPcm() {
        try {
            codec?.stop()
            codec?.release()
        } catch (e: Exception) {
            // Ignore cleanup errors
        }
        codec = null
        useRawPcm = true
        Log.i(TAG, "Switched to raw PCM mode")
    }

    private fun encodeRawPcm(pcmData: ShortArray, sampleCount: Int, outputBuffer: ByteArray): Int {
        // Simple raw PCM passthrough (little-endian)
        val byteCount = minOf(sampleCount * 2, outputBuffer.size)
        for (i in 0 until byteCount / 2) {
            val sample = pcmData[i]
            outputBuffer[i * 2] = (sample.toInt() and 0xFF).toByte()
            outputBuffer[i * 2 + 1] = (sample.toInt() shr 8 and 0xFF).toByte()
        }
        return byteCount
    }

    fun isUsingRawPcm(): Boolean = useRawPcm

    fun release() {
        try {
            codec?.let { c ->
                try {
                    c.flush()
                } catch (e: Exception) {
                    // Ignore flush errors
                }
                c.stop()
                c.release()
            }
            codec = null
        } catch (e: Exception) {
            Log.e(TAG, "Release error", e)
        }
        isInitialized = false
    }
}
