package com.mirage.capture.capture

import kotlin.math.min

/**
 * H.264/H.265 over RTP packetizer.
 * H.264: RFC 6184 (FU-A, type=28)
 * H.265: RFC 7798 (FU, type=49)
 * Supports Single NAL Unit and FU fragmentation.
 * Caches SPS/PPS (H.264) or VPS/SPS/PPS (H.265) and sends them before IDR frames.
 */
class RtpH264Packetizer(
    private val ssrc: Int = 0x12345678,
    private val payloadType: Int = 96,
    private val mtuPayload: Int = 1200,
    private val useHevc: Boolean = false
) {
    private var seq: Int = 0
    private var timestamp: Long = 0

    // SPS/PPS cache for IDR recovery (H.264 and H.265)
    private var vps: ByteArray? = null  // H.265 VPS (type=32)
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
     * Cache parameter set NALs for later use.
     * H.264: SPS(7), PPS(8)
     * H.265: VPS(32), SPS(33), PPS(34)
     */
    fun cacheParameterSets(nalPayload: ByteArray) {
        if (nalPayload.isEmpty()) return
        if (useHevc) {
            val nalType = (nalPayload[0].toInt() ushr 1) and 0x3F
            when (nalType) {
                32 -> vps = nalPayload.copyOf()  // VPS
                33 -> sps = nalPayload.copyOf()  // SPS
                34 -> pps = nalPayload.copyOf()  // PPS
            }
        } else {
            val nalType = nalPayload[0].toInt() and 0x1F
            when (nalType) {
                7 -> sps = nalPayload.copyOf()
                8 -> pps = nalPayload.copyOf()
            }
        }
    }

    /**
     * Send cached parameter sets before IDR.
     */
    fun sendCachedSpsPps(out: (ByteArray) -> Unit) {
        if (useHevc) {
            vps?.let { packetizeNalPayload(it, marker = false, out) }
        }
        sps?.let { packetizeNalPayload(it, marker = false, out) }
        pps?.let { packetizeNalPayload(it, marker = false, out) }
    }

    /**
     * Check if NAL is IDR.
     * H.264: type=5
     * H.265: type=19 (IDR_W_RADL) or type=20 (IDR_N_LP)
     */
    fun isIdr(nalPayload: ByteArray): Boolean {
        if (nalPayload.isEmpty()) return false
        return if (useHevc) {
            val nalType = (nalPayload[0].toInt() ushr 1) and 0x3F
            nalType == 19 || nalType == 20
        } else {
            (nalPayload[0].toInt() and 0x1F) == 5
        }
    }

    /**
     * Packetize a single NAL unit (without start code).
     * @param nal NAL payload without AnnexB start code
     * @param marker Set true for last NAL of access unit
     * @param out Callback to send RTP packet
     */
    fun packetizeNalPayload(nal: ByteArray, marker: Boolean, out: (ByteArray) -> Unit) {
        if (nal.isEmpty()) return

        // Single NAL Unit - fits in one packet (same for H.264 and H.265)
        if (nal.size <= mtuPayload) {
            out(buildRtpPacket(marker, nal))
            return
        }

        if (useHevc) {
            packetizeHevcFu(nal, marker, out)
        } else {
            packetizeH264FuA(nal, marker, out)
        }
    }

    /**
     * H.264 FU-A fragmentation (RFC 6184)
     */
    private fun packetizeH264FuA(nal: ByteArray, marker: Boolean, out: (ByteArray) -> Unit) {
        val nalHeader = nal[0].toInt() and 0xFF
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

    /**
     * H.265 FU fragmentation (RFC 7798)
     * HEVC NAL header is 2 bytes: [nal_unit_type(6b)|nuh_layer_id(6b)|nuh_temporal_id_plus1(3b)]
     * FU packet: PayloadHdr(2bytes) + FU_header(1byte) + fragment_data
     * PayloadHdr: nal_unit_type=49 (FU), layer_id=0, tid=1 → 0x62, 0x01
     * FU_header: S(1b)|E(1b)|FuType(6b) - FuType = original nal_unit_type
     */
    private fun packetizeHevcFu(nal: ByteArray, marker: Boolean, out: (ByteArray) -> Unit) {
        if (nal.size < 2) return

        // Extract HEVC nal_unit_type from NAL header byte[0]
        val fuType = ((nal[0].toInt() ushr 1) and 0x3F).toByte()
        // Preserve layer_id and tid from original NAL header
        val payloadHdr0 = (49 shl 1).toByte()  // nal_unit_type=49 (FU), forbidden=0, nuh_layer_id_msb=0
        val payloadHdr1 = nal[1]               // preserve nuh_layer_id_lsb and nuh_temporal_id_plus1

        var offset = 2  // Skip 2-byte HEVC NAL header
        val maxFrag = mtuPayload - 3  // PayloadHdr(2) + FU_header(1)
        var first = true

        while (offset < nal.size) {
            val remaining = nal.size - offset
            val chunk = min(remaining, maxFrag)

            val sBit = if (first) 0x80 else 0x00
            val eBit = if (offset + chunk >= nal.size) 0x40 else 0x00
            val fuHeader = (sBit or eBit or fuType.toInt()).toByte()

            val payload = ByteArray(3 + chunk)
            payload[0] = payloadHdr0
            payload[1] = payloadHdr1
            payload[2] = fuHeader
            System.arraycopy(nal, offset, payload, 3, chunk)

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
