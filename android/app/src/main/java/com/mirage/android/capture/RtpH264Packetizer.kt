package com.mirage.android.capture

import kotlin.math.min

/**
 * H.264 over RTP packetizer (RFC6184).
 * Supports Single NAL Unit and FU-A fragmentation.
 * Caches SPS/PPS and sends them before IDR frames.
 */
class RtpH264Packetizer(
    private val ssrc: Int = 0x12345678,
    private val payloadType: Int = 96,
    private val mtuPayload: Int = 1200
) {
    private var seq: Int = 0
    private var timestamp: Long = 0

    // SPS/PPS cache for IDR recovery
    private var sps: ByteArray? = null
    private var pps: ByteArray? = null

    /**
     * Set RTP timestamp (90kHz clock).
     * Call before packetizing each access unit.
     */
    fun setTimestamp90k(ts90k: Long) {
        timestamp = ts90k and 0xFFFFFFFFL
    }

    /**
     * Increment timestamp by delta (for fixed fps).
     * e.g., 3000 for 30fps (90000/30)
     */
    fun nextTimestamp90k(delta90k: Int) {
        timestamp = (timestamp + delta90k) and 0xFFFFFFFFL
    }

    /**
     * Cache SPS/PPS NALs for later use.
     * Call this for each NAL before packetizing.
     */
    fun cacheParameterSets(nalPayload: ByteArray) {
        if (nalPayload.isEmpty()) return
        val nalType = nalPayload[0].toInt() and 0x1F
        when (nalType) {
            7 -> sps = nalPayload.copyOf()
            8 -> pps = nalPayload.copyOf()
        }
    }

    /**
     * Send cached SPS/PPS before IDR.
     * Call this when IDR NAL is detected.
     */
    fun sendCachedSpsPps(out: (ByteArray) -> Unit) {
        sps?.let { packetizeNalPayload(it, marker = false, out) }
        pps?.let { packetizeNalPayload(it, marker = false, out) }
    }

    /**
     * Check if NAL is IDR (type 5)
     */
    fun isIdr(nalPayload: ByteArray): Boolean {
        if (nalPayload.isEmpty()) return false
        return (nalPayload[0].toInt() and 0x1F) == 5
    }

    /**
     * Packetize a single NAL unit (without start code).
     * @param nal NAL payload without AnnexB start code
     * @param marker Set true for last NAL of access unit
     * @param out Callback to send RTP packet
     */
    fun packetizeNalPayload(nal: ByteArray, marker: Boolean, out: (ByteArray) -> Unit) {
        if (nal.isEmpty()) return

        val nalHeader = nal[0].toInt() and 0xFF

        // Single NAL Unit - fits in one packet
        if (nal.size <= mtuPayload) {
            out(buildRtpPacket(marker, nal))
            return
        }

        // FU-A Fragmentation
        val f = nalHeader and 0x80
        val nri = nalHeader and 0x60
        val fuIndicator = (f or nri or 28).toByte()
        val originalType = nalHeader and 0x1F

        var offset = 1  // Skip NAL header byte
        val maxFrag = mtuPayload - 2  // FU indicator + FU header
        var first = true

        while (offset < nal.size) {
            val remaining = nal.size - offset
            val chunk = min(remaining, maxFrag)

            val sBit = if (first) 0x80 else 0x00
            val eBit = if (offset + chunk >= nal.size) 0x40 else 0x00
            val fuHeader = (sBit or eBit or originalType).toByte()

            val payload = ByteArray(2 + chunk)
            payload[0] = fuIndicator
            payload[1] = fuHeader
            System.arraycopy(nal, offset, payload, 2, chunk)

            val isLast = (offset + chunk >= nal.size)
            out(buildRtpPacket(marker && isLast, payload))

            offset += chunk
            first = false
        }
    }

    private fun buildRtpPacket(marker: Boolean, payload: ByteArray): ByteArray {
        val header = ByteArray(12)

        // V=2, P=0, X=0, CC=0
        header[0] = 0x80.toByte()

        // M + PT
        header[1] = ((if (marker) 0x80 else 0x00) or (payloadType and 0x7F)).toByte()

        // Sequence number (big endian)
        val seq16 = seq and 0xFFFF
        header[2] = (seq16 ushr 8).toByte()
        header[3] = (seq16 and 0xFF).toByte()
        seq = (seq + 1) and 0xFFFF

        // Timestamp (big endian)
        val ts = timestamp
        header[4] = ((ts ushr 24) and 0xFF).toByte()
        header[5] = ((ts ushr 16) and 0xFF).toByte()
        header[6] = ((ts ushr 8) and 0xFF).toByte()
        header[7] = (ts and 0xFF).toByte()

        // SSRC (big endian)
        header[8]  = ((ssrc ushr 24) and 0xFF).toByte()
        header[9]  = ((ssrc ushr 16) and 0xFF).toByte()
        header[10] = ((ssrc ushr 8) and 0xFF).toByte()
        header[11] = (ssrc and 0xFF).toByte()

        return header + payload
    }
}
