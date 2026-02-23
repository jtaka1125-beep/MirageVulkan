package com.mirage.capture.capture

/** AnnexB NAL splitter for byte streams containing 00 00 01 / 00 00 00 01 start codes. */
object AnnexB {
    fun splitNals(bytes: ByteArray): List<ByteArray> {
        val out = ArrayList<ByteArray>()
        var i = 0
        fun isStartCode(at: Int): Int {
            if (at + 3 <= bytes.size && bytes[at] == 0.toByte() && bytes[at+1] == 0.toByte()) {
                if (bytes[at+2] == 1.toByte()) return 3
                if (at + 4 <= bytes.size && bytes[at+2] == 0.toByte() && bytes[at+3] == 1.toByte()) return 4
            }
            return 0
        }
        // find first start code
        while (i < bytes.size) {
            val sc = isStartCode(i)
            if (sc > 0) { i += sc; break }
            i++
        }
        var nalStart = i
        while (i < bytes.size) {
            val sc = isStartCode(i)
            if (sc > 0) {
                val nalEnd = i
                if (nalEnd > nalStart) out.add(bytes.copyOfRange(nalStart, nalEnd))
                i += sc
                nalStart = i
                continue
            }
            i++
        }
        if (bytes.size > nalStart) out.add(bytes.copyOfRange(nalStart, bytes.size))
        return out
    }
}
