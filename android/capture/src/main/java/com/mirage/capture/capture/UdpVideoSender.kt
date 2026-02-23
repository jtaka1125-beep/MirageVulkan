package com.mirage.capture.capture

import android.util.Log
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/**
 * UDP sender for RTP packets over WiFi.
 * Implements VideoSender interface for interchangeability with UsbVideoSender.
 *
 * Address resolution is deferred to the send thread to avoid
 * NetworkOnMainThreadException.
 */
class UdpVideoSender(private val host: String, private val port: Int) : VideoSender {

    companion object {
        private const val TAG = "MirageUdpSender"
    }

    private val socket = DatagramSocket()
    @Volatile
    private var address: InetAddress? = null
    // Bounded queue: drop oldest packets when full (real-time video doesn't need old data)
    private val sendQueue = ArrayBlockingQueue<Runnable>(256)
    private val executor = ThreadPoolExecutor(1, 1, 0L, TimeUnit.MILLISECONDS, sendQueue,
        ThreadPoolExecutor.DiscardOldestPolicy())
    private val active = AtomicBoolean(true)

    init {
        socket.sendBufferSize = 2 * 1024 * 1024
        Log.i(TAG, "UDP sender initialized: $host:$port (queue limit: 256)")
    }

    private fun resolveAddress(): InetAddress? {
        if (address == null) {
            try {
                address = InetAddress.getByName(host)
            } catch (e: Exception) {
                Log.e(TAG, "Failed to resolve host: $host", e)
                active.set(false)
            }
        }
        return address
    }

    override fun send(rtpPacket: ByteArray) {
        if (!active.get()) return
        executor.execute {
            try {
                val addr = resolveAddress() ?: return@execute
                val pkt = DatagramPacket(rtpPacket, rtpPacket.size, addr, port)
                socket.send(pkt)
            } catch (e: Exception) {
                Log.e(TAG, "Send error", e)
                active.set(false)
            }
        }
    }

    override fun isActive(): Boolean = active.get() && !socket.isClosed

    override fun close() {
        active.set(false)
        executor.shutdown()
        socket.close()
        Log.i(TAG, "UDP sender closed")
    }
}
