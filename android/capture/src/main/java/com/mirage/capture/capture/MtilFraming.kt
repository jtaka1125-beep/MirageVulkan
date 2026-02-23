package com.mirage.capture.capture

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Minimal framing for tile streaming.
 *
 * StreamHello (sent once per TCP connection):
 *  magic "MSH1" (4)
 *  ver (1)
 *  codec (1) 0=avc 1=hevc
 *  tileIndex (1)
 *  tilesX (1)
 *  tilesY (1)
 *  targetW (2)
 *  targetH (2)
 *  tileW (2)
 *  tileH (2)
 *  reserved (2)
 *  total = 4+1+1+1+1+1+1+2+2+2+2+2 = 20 bytes
 *
 * MTIL per-packet header:
 *  magic "MTIL" (4)
 *  ver (1)
 *  tileIndex (1)
 *  frameId (4)
 *  timestampMs (8)
 *  payloadLen (4)
 *  total = 22 bytes
 */
object MtilFraming {
    const val CODEC_AVC: Byte = 0
    const val CODEC_HEVC: Byte = 1

    fun buildHello(
        codec: Byte,
        tileIndex: Int,
        tilesX: Int,
        tilesY: Int,
        targetW: Int,
        targetH: Int,
        tileW: Int,
        tileH: Int,
    ): ByteArray {
        val bb = ByteBuffer.allocate(20).order(ByteOrder.BIG_ENDIAN)
        bb.put(byteArrayOf('M'.code.toByte(), 'S'.code.toByte(), 'H'.code.toByte(), '1'.code.toByte()))
        bb.put(1) // ver
        bb.put(codec)
        bb.put(tileIndex.toByte())
        bb.put(tilesX.toByte())
        bb.put(tilesY.toByte())
        bb.putShort(targetW.toShort())
        bb.putShort(targetH.toShort())
        bb.putShort(tileW.toShort())
        bb.putShort(tileH.toShort())
        bb.putShort(0) // reserved
        return bb.array()
    }

    fun buildMtilHeader(tileIndex: Int, frameId: Int, timestampMs: Long, payloadLen: Int): ByteArray {
        val bb = ByteBuffer.allocate(22).order(ByteOrder.BIG_ENDIAN)
        bb.put(byteArrayOf('M'.code.toByte(), 'T'.code.toByte(), 'I'.code.toByte(), 'L'.code.toByte()))
        bb.put(1) // ver
        bb.put(tileIndex.toByte())
        bb.putInt(frameId)
        bb.putLong(timestampMs)
        bb.putInt(payloadLen)
        return bb.array()
    }
}
