package com.mirage.accessory.usb

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Mirage USB Protocol (matches PC side mirage/usb/protocol.hpp)
 *
 * Header (14 bytes):
 *   magic:   4 bytes (0x4D495241 = "MIRA" LE)
 *   version: 1 byte  (1)
 *   cmd:     1 byte
 *   seq:     4 bytes
 *   len:     4 bytes (payload length)
 *
 * Commands:
 *   PING=0, TAP=1, BACK=2, KEY=3, ACK=0x80
 */
object Protocol {
    const val MAGIC = 0x4D495241
    const val VERSION: Byte = 1
    const val HEADER_SIZE = 14

    const val CMD_PING: Byte = 0
    const val CMD_TAP: Byte = 1
    const val CMD_BACK: Byte = 2
    const val CMD_KEY: Byte = 3
    const val CMD_CONFIG: Byte = 0x04      // 設定変更: PC -> Android
    const val CMD_CLICK_ID: Byte = 0x05    // リソースID指定タップ: PC -> Android
    const val CMD_CLICK_TEXT: Byte = 0x06  // テキスト指定タップ: PC -> Android
    const val CMD_SWIPE: Byte = 0x07
    const val CMD_PINCH: Byte = 0x08
    const val CMD_LONGPRESS: Byte = 0x09
    const val CMD_AUDIO_FRAME: Byte = 0x10  // Audio frame: Android -> PC
    const val CMD_VIDEO_FPS: Byte = 0x24    // FPS change command: PC -> Android
    const val CMD_VIDEO_ROUTE: Byte = 0x25  // Video route switch: PC -> Android
    const val CMD_VIDEO_IDR: Byte = 0x26     // IDR request: PC -> Android
    const val CMD_DEVICE_INFO: Byte = 0x27  // Device info: Android -> PC (hardware_id)
    const val CMD_ACK: Byte = 0x80.toByte()

    const val STATUS_OK: Byte = 0
    const val STATUS_ERR_UNKNOWN_CMD: Byte = 1
    const val STATUS_ERR_INVALID_PAYLOAD: Byte = 2
    const val STATUS_ERR_BUSY: Byte = 3

    data class Header(
        val magic: Int,
        val version: Byte,
        val cmd: Byte,
        val seq: Int,
        val payloadLen: Int
    ) {
        fun isValid(): Boolean = magic == MAGIC && version == VERSION
    }

    data class TapPayload(
        val x: Int,
        val y: Int,
        val w: Int,
        val h: Int,
        val flags: Int
    )

    data class BackPayload(val flags: Int)

    data class KeyPayload(val keycode: Int, val flags: Int)

    data class SwipePayload(
        val startX: Int, val startY: Int,
        val endX: Int, val endY: Int,
        val durationMs: Int, val flags: Int
    )

    data class PinchPayload(
        val centerX: Int, val centerY: Int,
        val startDistance: Int, val endDistance: Int,
        val durationMs: Int, val flags: Int,
        val angle: Int
    )

    data class LongPressPayload(
        val x: Int, val y: Int,
        val durationMs: Int
    )

    // リソースID指定タップのペイロード
    data class ClickIdPayload(val resourceId: String)

    // テキスト指定タップのペイロード
    data class ClickTextPayload(val text: String)

    sealed class Command {
        abstract val seq: Int

        data class Ping(override val seq: Int) : Command()
        data class Tap(override val seq: Int, val x: Int, val y: Int, val w: Int, val h: Int) : Command()
        data class Back(override val seq: Int) : Command()
        data class Key(override val seq: Int, val keycode: Int) : Command()
        data class Swipe(override val seq: Int, val startX: Int, val startY: Int, val endX: Int, val endY: Int, val durationMs: Int) : Command()
        data class Pinch(override val seq: Int, val centerX: Int, val centerY: Int, val startDistance: Int, val endDistance: Int, val durationMs: Int, val angleDeg100: Int) : Command()
        data class LongPress(override val seq: Int, val x: Int, val y: Int, val durationMs: Int) : Command()
        data class Config(override val seq: Int, val payload: ByteArray) : Command()           // 設定変更
        data class ClickId(override val seq: Int, val resourceId: String) : Command()           // リソースID指定タップ
        data class ClickText(override val seq: Int, val text: String) : Command()               // テキスト指定タップ
        data class VideoFps(override val seq: Int, val targetFps: Int) : Command()
        data class VideoRoute(override val seq: Int, val mode: Int, val host: String, val port: Int) : Command()
        data class VideoIdr(override val seq: Int) : Command()
        data class Unknown(override val seq: Int, val cmd: Byte) : Command()
    }

    // VideoRoute mode constants
    const val VIDEO_ROUTE_USB = 0
    const val VIDEO_ROUTE_WIFI = 1

    fun parseHeader(data: ByteArray, offset: Int = 0): Header? {
        if (data.size - offset < HEADER_SIZE) return null
        val buf = ByteBuffer.wrap(data, offset, HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN)
        return Header(
            magic = buf.int,
            version = buf.get(),
            cmd = buf.get(),
            seq = buf.int,
            payloadLen = buf.int
        )
    }

    fun parseCommand(data: ByteArray): Command? {
        val header = parseHeader(data) ?: return null
        if (!header.isValid()) return null

        val payloadOffset = HEADER_SIZE
        val payloadData = if (header.payloadLen > 0 && data.size >= payloadOffset + header.payloadLen) {
            ByteBuffer.wrap(data, payloadOffset, header.payloadLen).order(ByteOrder.LITTLE_ENDIAN)
        } else null

        return when (header.cmd) {
            CMD_PING -> Command.Ping(header.seq)

            CMD_TAP -> {
                if (payloadData == null || header.payloadLen < 20) return null
                Command.Tap(
                    seq = header.seq,
                    x = payloadData.int,
                    y = payloadData.int,
                    w = payloadData.int,
                    h = payloadData.int
                )
            }

            CMD_BACK -> Command.Back(header.seq)

            CMD_KEY -> {
                if (payloadData == null || header.payloadLen < 8) return null
                Command.Key(
                    seq = header.seq,
                    keycode = payloadData.int
                )
            }

            CMD_SWIPE -> {
                if (payloadData == null || header.payloadLen < 20) return null
                Command.Swipe(
                    seq = header.seq,
                    startX = payloadData.int,
                    startY = payloadData.int,
                    endX = payloadData.int,
                    endY = payloadData.int,
                    durationMs = payloadData.int
                )
            }

            CMD_PINCH -> {
                if (payloadData == null || header.payloadLen < 24) return null
                val centerX = payloadData.int
                val centerY = payloadData.int
                val startDist = payloadData.int
                val endDist = payloadData.int
                val durMs = payloadData.int
                // (reserved field removed FIX-B: payload now 24 bytes, 6 ints)
                val angleDeg100 = payloadData.int
                Command.Pinch(header.seq, centerX, centerY, startDist, endDist, durMs, angleDeg100)
            }

            CMD_LONGPRESS -> {
                if (payloadData == null || header.payloadLen < 12) return null
                Command.LongPress(
                    seq = header.seq,
                    x = payloadData.int,
                    y = payloadData.int,
                    durationMs = payloadData.int
                )
            }

            CMD_CONFIG -> {
                // 設定変更: ペイロード全体をバイト配列として渡す
                val raw = if (payloadData != null) {
                    val b = ByteArray(header.payloadLen)
                    payloadData.get(b)
                    b
                } else ByteArray(0)
                Command.Config(seq = header.seq, payload = raw)
            }

            CMD_CLICK_ID -> {
                // リソースID指定タップ: ペイロードはUTF-8文字列
                if (payloadData == null || header.payloadLen < 1) return null
                val idBytes = ByteArray(header.payloadLen)
                payloadData.get(idBytes)
                Command.ClickId(
                    seq = header.seq,
                    resourceId = String(idBytes, Charsets.UTF_8).trimEnd('\u0000')
                )
            }

            CMD_CLICK_TEXT -> {
                // テキスト指定タップ: ペイロードはUTF-8文字列
                if (payloadData == null || header.payloadLen < 1) return null
                val textBytes = ByteArray(header.payloadLen)
                payloadData.get(textBytes)
                Command.ClickText(
                    seq = header.seq,
                    text = String(textBytes, Charsets.UTF_8).trimEnd('\u0000')
                )
            }

            CMD_VIDEO_FPS -> {
                if (payloadData == null || header.payloadLen < 4) return null
                Command.VideoFps(
                    seq = header.seq,
                    targetFps = payloadData.int
                )
            }

            CMD_VIDEO_ROUTE -> {
                if (payloadData == null || header.payloadLen < 8) return null
                val mode = payloadData.int
                val port = payloadData.int
                // Host IP is remaining bytes as string (null-terminated or full length)
                val hostBytes = ByteArray(header.payloadLen - 8)
                if (hostBytes.isNotEmpty()) {
                    payloadData.get(hostBytes)
                }
                val host = String(hostBytes).trimEnd('\u0000')
                Command.VideoRoute(
                    seq = header.seq,
                    mode = mode,
                    host = host,
                    port = port
                )
            }

            CMD_VIDEO_IDR -> {
                Command.VideoIdr(seq = header.seq)
            }

            else -> Command.Unknown(header.seq, header.cmd)
        }
    }

    fun buildAck(seq: Int, status: Byte): ByteArray {
        val buf = ByteBuffer.allocate(HEADER_SIZE + 8).order(ByteOrder.LITTLE_ENDIAN)
        // Header
        buf.putInt(MAGIC)
        buf.put(VERSION)
        buf.put(CMD_ACK)
        buf.putInt(seq)
        buf.putInt(8) // payload len
        // AckPayload
        buf.putInt(seq)
        buf.put(status)
        buf.put(0) // reserved
        buf.put(0)
        buf.put(0)
        return buf.array()
    }

    /**
     * Build DEVICE_INFO packet to send hardware_id to PC.
     * This allows PC to map USB serial -> ADB hardware_id.
     *
     * @param hardwareId The device's unique hardware ID (android_id_serialno)
     * @return MIRA packet with DEVICE_INFO command
     */
    fun buildDeviceInfo(hardwareId: String): ByteArray {
        val idBytes = hardwareId.toByteArray(Charsets.UTF_8)
        val buf = ByteBuffer.allocate(HEADER_SIZE + idBytes.size).order(ByteOrder.LITTLE_ENDIAN)
        // Header
        buf.putInt(MAGIC)
        buf.put(VERSION)
        buf.put(CMD_DEVICE_INFO)
        buf.putInt(0) // seq (not used for device info)
        buf.putInt(idBytes.size)
        // Payload: hardware_id string
        buf.put(idBytes)
        return buf.array()
    }
    /**
     * Build audio frame packet for USB transmission
     * Payload: timestamp (4 bytes) + opus data (N bytes)
     */
    fun buildAudioFrame(seq: Int, timestamp: Int, opusData: ByteArray): ByteArray {
        val payloadLen = 4 + opusData.size
        val buf = ByteBuffer.allocate(HEADER_SIZE + payloadLen).order(ByteOrder.LITTLE_ENDIAN)
        // Header
        buf.putInt(MAGIC)
        buf.put(VERSION)
        buf.put(CMD_AUDIO_FRAME)
        buf.putInt(seq)
        buf.putInt(payloadLen)
        // Payload
        buf.putInt(timestamp)
        buf.put(opusData)
        return buf.array()
    }

    // =============================================================================
    // VID0 Video Protocol (for USB video streaming)
    // =============================================================================

    /** VID0 magic bytes: "VID0" in big-endian */
    const val VIDEO_MAGIC = 0x56494430  // 'V' 'I' 'D' '0'
    const val VIDEO_HEADER_SIZE = 8     // magic(4) + length(4)

    /**
     * Build video frame packet with VID0 framing for USB transmission.
     * Format: [VID0(4 bytes, BE)][length(4 bytes, BE)][RTP data]
     *
     * This framing is compatible with PC-side usb_video_receiver parsing.
     *
     * @param rtpData Complete RTP packet (header + payload)
     * @return VID0-framed packet ready for USB transmission
     */
    fun buildVideoFrame(rtpData: ByteArray): ByteArray {
        val packet = ByteArray(VIDEO_HEADER_SIZE + rtpData.size)

        // Magic "VID0" (big-endian)
        packet[0] = 0x56  // 'V'
        packet[1] = 0x49  // 'I'
        packet[2] = 0x44  // 'D'
        packet[3] = 0x30  // '0'

        // Length (big-endian)
        val len = rtpData.size
        packet[4] = ((len shr 24) and 0xFF).toByte()
        packet[5] = ((len shr 16) and 0xFF).toByte()
        packet[6] = ((len shr 8) and 0xFF).toByte()
        packet[7] = (len and 0xFF).toByte()

        // RTP data
        System.arraycopy(rtpData, 0, packet, VIDEO_HEADER_SIZE, rtpData.size)
        return packet
    }
}
