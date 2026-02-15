package com.mirage.android.svc

import android.util.Log
import com.mirage.android.core.Config
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread

class UdpSender(
    private val host: String = Config.UDP_HOST,
    private val port: Int = Config.UDP_PORT,
) {
    companion object {
        private const val TAG = "MirageUdpSender"
    }

    private val q = LinkedBlockingQueue<String>(4096)
    @Volatile private var running = false
    private var th: Thread? = null

    fun start() {
        if (running) return
        running = true
        th = thread(name = "mirage-udp-sender", isDaemon = true) {
            try {
                val addr = InetAddress.getByName(host)
                DatagramSocket().use { sock ->
                    while (running) {
                        // Use poll with timeout instead of take to allow clean shutdown
                        val line = q.poll(500, TimeUnit.MILLISECONDS) ?: continue
                        val bytes = line.toByteArray(Charsets.UTF_8)
                        val p = DatagramPacket(bytes, bytes.size, addr, port)
                        sock.send(p)
                    }
                }
            } catch (e: InterruptedException) {
                Log.d(TAG, "UDP sender interrupted")
            } catch (e: Exception) {
                Log.e(TAG, "UDP sender error", e)
            } finally {
                running = false
            }
        }
    }

    fun stop() {
        running = false
        th?.interrupt()
        try {
            th?.join(1000)
        } catch (e: InterruptedException) {
            // ignore
        }
        th = null
        q.clear()
    }

    fun trySendLine(tag: String, jsonPayload: String) {
        // 1 datagram = 1 line
        val line = "$tag\t$jsonPayload"
        q.offer(line)
    }
}
