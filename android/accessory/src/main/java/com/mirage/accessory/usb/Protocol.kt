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
    const val CMD_UI_TREE_REQ: Byte = 0x0A  // UIツリー要求: PC -> Android
    const val CMD_UI_TREE_DATA: Byte = 0x0B // UIツリー応答: Android -> PC (JSON)
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
    const val STATUS_ERR_NOT_FOUND: Byte = 4

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
        data class Swipe(override val seq: Int, val startX: Int, val startY: Int, val endX: Int, val endY: Int, val durationMs: Int, val screenW: Int = 0, val screenH: Int = 0) : Command()
        data class Pinch(override val seq: Int, val centerX: Int, val centerY: Int, val startDistance: Int, val endDistance: Int, val durationMs: Int, val angleDeg100: Int) : Command()
        data class LongPress(override val seq: Int, val x: Int, val y: Int, val durationMs: Int) : Command()
        data class Config(override val seq: Int, val payload: ByteArray) : Command()
        data class ClickId(override val seq: Int, val resourceId: String) : Command()
        data class ClickText(override val seq: Int, val text: String) : Command()
        data class UiTreeReq(override val seq: Int) : Command()           // UIツリー要求
        data class VideoFps(override val seq: Int, val targetFps: Int) : Command()
        data class VideoRoute(override val seq: Int, val mode: Int, val host: String, val port: Int) : Command()
        data class VideoIdr(override val seq: Int) : Command()
    }

    // -------------------------------------------------------------------------
    // Header builder
    // -------------------------------------------------------------------------
    fun buildHeader(cmd: Byte, seq: Int, payloadLen: Int): ByteArray {
        val buf = ByteBuffer.allocate(HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN)
        buf.putInt(MAGIC)
        buf.put(VERSION)
        buf.put(cmd)
        buf.putInt(seq)
        buf.putInt(payloadLen)
        return buf.array()
    }

    // -------------------------------------------------------------------------
    // Header parser
    // -------------------------------------------------------------------------
    fun parseHeader(data: ByteArray, offset: Int = 0): Header? {
        if (data.size - offset < HEADER_SIZE) return null
        val buf = ByteBuffer.wrap(data, offset, HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN)
        val magic = buf.int
        if (magic != MAGIC) return null
        val version = buf.get()
        val cmd = buf.get()
        val seq = buf.int
        val payloadLen = buf.int
        return Header(magic, version, cmd, seq, payloadLen)
    }

    // -------------------------------------------------------------------------
    // Full packet builder
    // -------------------------------------------------------------------------
    fun buildPacket(cmd: Byte, seq: Int, payload: ByteArray? = null): ByteArray {
        val payloadLen = payload?.size ?: 0
        val buf = ByteBuffer.allocate(HEADER_SIZE + payloadLen).order(ByteOrder.LITTLE_ENDIAN)
        buf.putInt(MAGIC)
        buf.put(VERSION)
        buf.put(cmd)
        buf.putInt(seq)
        buf.putInt(payloadLen)
        if (payload != null) buf.put(payload)
        return buf.array()
    }
}
