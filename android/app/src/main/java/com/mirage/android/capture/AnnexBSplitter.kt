package com.mirage.android.capture

/**
 * Splits AnnexB byte stream into individual NAL units.
 * Returns NAL payloads WITHOUT start codes.
 */
object AnnexBSplitter {

    fun splitToNals(data: ByteArray): List<ByteArray> {
        val indices = ArrayList<Int>()
        var i = 0

        // Find all start code positions
        while (i + 2 < data.size) {
            val is4Byte = (i + 3 < data.size &&
                    data[i] == 0.toByte() && data[i + 1] == 0.toByte() &&
                    data[i + 2] == 0.toByte() && data[i + 3] == 1.toByte())
            val is3Byte = (!is4Byte &&
                    data[i] == 0.toByte() && data[i + 1] == 0.toByte() &&
                    data[i + 2] == 1.toByte())

            if (is4Byte) {
                indices.add(i)
                i += 4
                continue
            }
            if (is3Byte) {
                indices.add(i)
                i += 3
                continue
            }
            i++
        }

        if (indices.isEmpty()) {
            // No start code found: treat entire data as single NAL
            return listOf(data)
        }

        val out = ArrayList<ByteArray>()
        for (k in indices.indices) {
            val scPos = indices[k]
            // Determine start code length (3 or 4 bytes)
            val scLen = if (scPos + 3 < data.size &&
                data[scPos + 2] == 0.toByte() && data[scPos + 3] == 1.toByte()) 4 else 3

            val nalStart = scPos + scLen
            val nalEnd = if (k + 1 < indices.size) indices[k + 1] else data.size

            if (nalStart < nalEnd) {
                out.add(data.copyOfRange(nalStart, nalEnd))
            }
        }

        return out
    }

    /**
     * Get NAL type from NAL payload (first byte & 0x1F)
     */
    fun getNalType(nal: ByteArray): Int {
        return if (nal.isNotEmpty()) nal[0].toInt() and 0x1F else 0
    }
}
