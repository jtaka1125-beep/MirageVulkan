package com.mirage.capture.capture

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.media.projection.MediaProjection
import android.util.Log
import android.view.Surface
import kotlin.math.ceil

/**
 * Generic tiling encoder (device-agnostic).
 *
 * - Creates ONE VirtualDisplay at targetW x targetH feeding TileRepeater.
 * - TileRepeater crops the frame into tiles and renders into each encoder input surface.
 * - Each tile is encoded independently and sent via TcpVideoSender on tcpBasePort+tileIndex.
 * - RTP timestamps are derived from MediaCodec presentationTimeUs (90kHz), so tiles stay in sync
 *   as long as TileRepeater applies the same PTS to all tiles.
 *
 * NOTE: This build uses TcpVideoSender's minimal signature (port, onClientConnected). No MTIL.
 */
class TiledEncoder(
    private val projection: MediaProjection,
    private val densityDpi: Int,
    private val targetW: Int,
    private val targetH: Int,
    private val fps: Int,
    private val bitrate: Int,
    private val tcpBasePort: Int,
    private val useHevc: Boolean,
    private val onRequestIdr: () -> Unit,
) {
    companion object {
        private const val TAG = "TiledEncoder"
        private const val M_AVC = MediaFormat.MIMETYPE_VIDEO_AVC
        private const val M_HEVC = MediaFormat.MIMETYPE_VIDEO_HEVC

        private fun isSoftwareCodecName(name: String): Boolean {
            val n = name.lowercase()
            return n.contains("c2.android") || n.contains("omx.google")
        }

        private fun align16(x: Int): Int = (x / 16) * 16

        data class TilePlan(
            val mime: String,
            val codecName: String,
            val tilesX: Int,
            val tilesY: Int,
            val tileW: Int,
            val tileH: Int,
            val maxW: Int,
            val maxH: Int,
        )

        /**
         * Choose a tiling plan when portrait size (targetW x targetH) does NOT fit as-is.
         * If it already fits without tiling, returns null (caller can use single encoder).
         */
        fun chooseTilePlan(targetW: Int, targetH: Int, useHevc: Boolean): TilePlan? {
            val mime = if (useHevc) M_HEVC else M_AVC
            val list = MediaCodecList(MediaCodecList.REGULAR_CODECS)

            var best: TilePlan? = null
            var bestArea = -1L

            for (info in list.codecInfos) {
                if (!info.isEncoder) continue
                if (isSoftwareCodecName(info.name)) continue
                if (!info.supportedTypes.any { it.equals(mime, ignoreCase = true) }) continue

                val caps = runCatching { info.getCapabilitiesForType(mime) }.getOrNull() ?: continue
                val vcap = caps.videoCapabilities ?: continue

                // If portrait fits, no tiling needed for this codec
                if (targetH < 2000 && vcap.isSizeSupported(targetW, targetH)) continue

                val maxW = vcap.supportedWidths.upper
                val maxH = vcap.supportedHeights.upper
                val safeW = (maxW * 0.95).toInt().coerceAtLeast(320)
                val safeH = (maxH * 0.95).toInt().coerceAtLeast(320)

                var tilesX = ceil(targetW.toDouble() / safeW.toDouble()).toInt().coerceAtLeast(1)
                var tilesY = ceil(targetH.toDouble() / safeH.toDouble()).toInt().coerceAtLeast(2)

                var tileW = align16(ceil(targetW.toDouble() / tilesX).toInt())
                var tileH = align16(ceil(targetH.toDouble() / tilesY).toInt())

                var guard = 0
                while (guard++ < 12 && !vcap.isSizeSupported(tileW, tileH)) {
                    if (tileH > safeH) tilesY++ else tilesX++
                    tileW = align16(ceil(targetW.toDouble() / tilesX).toInt())
                    tileH = align16(ceil(targetH.toDouble() / tilesY).toInt())
                }

                if (!vcap.isSizeSupported(tileW, tileH)) continue

                val area = maxW.toLong() * maxH.toLong()
                if (area > bestArea) {
                    bestArea = area
                    best = TilePlan(mime, info.name, tilesX, tilesY, tileW, tileH, maxW, maxH)
                }
            }
            return best
        }
    }

    private var plan: TilePlan? = null
    private var virtualDisplay: android.hardware.display.VirtualDisplay? = null
    private var tileRepeater: TileRepeater? = null

    private val codecs = mutableListOf<MediaCodec>()
    private val encoderSurfaces = mutableListOf<Surface>()
    private val senders = mutableListOf<TcpVideoSender>()
    private val packetizers = mutableListOf<RtpH264Packetizer>()

    @Volatile private var running = false
    private val drainThreads = mutableListOf<Thread>()

    fun start(): Boolean {
        plan = chooseTilePlan(targetW, targetH, useHevc)
        val p = plan ?: run {
            Log.i(TAG, "No tiling plan needed/available")
            return false
        }

        Log.i(TAG, "TilePlan codec=${p.codecName} mime=${p.mime} max=${p.maxW}x${p.maxH} tiles=${p.tilesX}x${p.tilesY} tile=${p.tileW}x${p.tileH} target=${targetW}x${targetH}")

        val tileCount = p.tilesX * p.tilesY
        for (i in 0 until tileCount) {
            val codec = MediaCodec.createByCodecName(p.codecName)
            val fmt = MediaFormat.createVideoFormat(p.mime, p.tileW, p.tileH).apply {
                setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
                setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
                setInteger(MediaFormat.KEY_FRAME_RATE, fps)
                setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
            }
            codec.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            val surf = codec.createInputSurface()

            val sender = TcpVideoSender(
                port = tcpBasePort + i,
                onClientConnected = { onRequestIdr() }
            )

            codecs.add(codec)
            encoderSurfaces.add(surf)
            senders.add(sender)
            packetizers.add(RtpH264Packetizer(ssrc = 0x12345678 + i))
        }

        // Start codecs
        for (c in codecs) c.start()

        // Start TileRepeater (crops into tile surfaces)
        val outs = ArrayList<TileRepeater.TileOutput>(tileCount)
        for (y in 0 until p.tilesY) {
            for (x in 0 until p.tilesX) {
                val idx = y * p.tilesX + x
                outs.add(
                    TileRepeater.TileOutput(
                        tileIndex = idx,
                        tilesX = p.tilesX,
                        tilesY = p.tilesY,
                        tileX = x,
                        tileY = y,
                        tileW = p.tileW,
                        tileH = p.tileH,
                        outputSurface = encoderSurfaces[idx]
                    )
                )
            }
        }

        tileRepeater = TileRepeater(outs, targetW, targetH, fps)
        tileRepeater?.start()
        val inSurf = tileRepeater?.inputSurface ?: run {
            Log.e(TAG, "TileRepeater input surface not ready")
            stop()
            return false
        }

        virtualDisplay = projection.createVirtualDisplay(
            "MirageCapture",
            targetW,
            targetH,
            densityDpi,
            0,
            inSurf,
            null,
            null
        )

        running = true
        for (i in codecs.indices) {
            val th = Thread({ drainLoop(i) }, "TileDrain-$i")
            drainThreads.add(th)
            th.start()
        }

        return true
    }

    fun stop() {
        running = false
        for (t in drainThreads) {
            runCatching { t.join(1500) }
        }
        drainThreads.clear()

        runCatching { virtualDisplay?.release() }
        virtualDisplay = null

        runCatching { tileRepeater?.stop() }
        tileRepeater = null

        for (c in codecs) {
            runCatching { c.stop() }
            runCatching { c.release() }
        }
        codecs.clear()

        for (s in senders) {
            runCatching { s.close() }
        }
        senders.clear()

        encoderSurfaces.clear()
        packetizers.clear()
    }

    private fun drainLoop(tileIndex: Int) {
        val codec = codecs[tileIndex]
        val sender = senders[tileIndex]
        val packetizer = packetizers[tileIndex]
        val info = MediaCodec.BufferInfo()

        while (running) {
            val outIndex = codec.dequeueOutputBuffer(info, 10_000)
            if (outIndex >= 0) {
                val outBuf = codec.getOutputBuffer(outIndex)
                if (outBuf != null && info.size > 0) {
                    val data = ByteArray(info.size)
                    outBuf.position(info.offset)
                    outBuf.limit(info.offset + info.size)
                    outBuf.get(data)

                    val ts90k = (info.presentationTimeUs * 90L) / 1000L
                    packetizer.setTimestamp90k(ts90k)

                    val nals = AnnexB.splitNals(data)
                    for ((idx, nal) in nals.withIndex()) {
                        packetizer.cacheParameterSets(nal)
                        if (packetizer.isIdr(nal)) {
                            packetizer.sendCachedSpsPps { pkt -> sender.send(pkt) }
                        }
                        val marker = (idx == nals.lastIndex)
                        packetizer.packetizeNalPayload(nal, marker) { pkt -> sender.send(pkt) }
                    }
                }
                codec.releaseOutputBuffer(outIndex, false)
            }
        }
    }
}

