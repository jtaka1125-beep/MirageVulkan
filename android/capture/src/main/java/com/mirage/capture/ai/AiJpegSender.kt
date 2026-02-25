package com.mirage.capture.ai

import android.util.Log
import java.io.BufferedOutputStream
import java.io.DataOutputStream
import java.net.InetSocketAddress
import java.net.Socket

/**
 * Separate AI stream sender (JPEG frames) to avoid collision with GUI video ports.
 * Frame format: [int32 len][int32 w][int32 h][int64 tsUs][bytes jpeg]
 */
class AiJpegSender(private val host: String, private val port: Int) {
    private val TAG = "AiJpegSender"

    private var sock: Socket? = null
    private var out: DataOutputStream? = null

    fun connect(timeoutMs: Int = 2000): Boolean {
        return try {
            close()
            val s = Socket()
            s.tcpNoDelay = true
            s.connect(InetSocketAddress(host, port), timeoutMs)
            sock = s
            out = DataOutputStream(BufferedOutputStream(s.getOutputStream()))
            Log.i(TAG, "Connected to $host:$port")
            true
        } catch (e: Exception) {
            Log.w(TAG, "Connect failed: ${e.message}")
            close()
            false
        }
    }

    fun send(jpeg: ByteArray, w: Int, h: Int, tsUs: Long): Boolean {
        if (out == null) {
            if (!connect()) return false
        }
        val o = out ?: return false
        return try {
            o.writeInt(jpeg.size)
            o.writeInt(w)
            o.writeInt(h)
            o.writeLong(tsUs)
            o.write(jpeg)
            o.flush()
            true
        } catch (e: Exception) {
            Log.w(TAG, "Send failed: ${e.message}")
            close()
            false
        }
    }

    fun close() {
        try { out?.close() } catch (_: Exception) {}
        try { sock?.close() } catch (_: Exception) {}
        out = null
        sock = null
    }
}
