package com.mirage.android.capture

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
 */
class UdpVideoSender(host: String, private val port: Int) : VideoSender {

    companion object {
        private const val TAG = "MirageUdpSender"
    }

    private val socket = DatagramSocket()
    private val address: InetAddress = InetAddress.getByName(host)
    // Bounded queue: drop oldest packets when full (real-time video doesn't need old data)
    private val sendQueue = ArrayBlockingQueue<Runnable>(256)
    private val executor = ThreadPoolExecutor(1, 1, 0L, TimeUnit.MILLISECONDS, sendQueue,
        ThreadPoolExecutor.DiscardOldestPolicy())
    private val active = AtomicBoolean(true)

    init {
        socket.sendBufferSize = 2 * 1024 * 1024
        Log.i(TAG, "UDP sender initialized: $host:$port (queue limit: 256)")
    }

    override fun send(rtpPacket: ByteArray) {
        if (!active.get()) return
        executor.execute {
            try {
                val pkt = DatagramPacket(rtpPacket, rtpPacket.size, address, port)
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
