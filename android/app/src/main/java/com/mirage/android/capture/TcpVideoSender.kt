package com.mirage.android.capture

import android.util.Log
import com.mirage.android.usb.Protocol
import java.io.OutputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong

/**
 * TCP video sender for ADB forward mode.
 *
 * Acts as a TCP SERVER on localhost:port. The PC connects via
 * `adb forward tcp:PORT tcp:PORT` and receives VID0-framed RTP packets
 * (same format as UsbVideoSender), so the PC-side can reuse
 * the exact same RTP → H264 decoder pipeline.
 *
 * Thread model:
 * - Encoder thread calls send() which enqueues packets (non-blocking)
 * - Accept thread waits for client connections
 * - Writer thread dequeues and writes packets to the connected client
 *
 * If the client disconnects, the writer thread cleans up and the accept
 * thread waits for a new connection. The ServerSocket stays alive until close().
 *
 * @param port TCP port to listen on (default 50100)
 */
class TcpVideoSender(
    private val port: Int = 50100,
    private val onClientConnected: (() -> Unit)? = null
) : VideoSender {

    companion object {
        private const val TAG = "TcpVideoSender"
        private const val QUEUE_CAPACITY = 256
        private const val STATS_INTERVAL = 100L
    }

    private val active = AtomicBoolean(true)
    private val packetsSent = AtomicLong(0)
    private val bytesSent = AtomicLong(0)

    // Packet queue: encoder thread → writer thread (non-blocking for encoder)
    private val sendQueue = ArrayBlockingQueue<ByteArray>(QUEUE_CAPACITY)

    // Shared state for client connection
    @Volatile
    private var clientConnected = false

    private val serverSocket: ServerSocket
    private val acceptThread: Thread
    private val writerThread: Thread

    // Current client socket and output stream, guarded by @Volatile + synchronized close
    @Volatile
    private var clientSocket: Socket? = null
    @Volatile
    private var clientOutput: OutputStream? = null

    init {
        serverSocket = // Bind without forcing IPv4 loopback to allow IPv4/IPv6 (dual-stack) ADB forward connections
            ServerSocket(port)
        Log.i(TAG, "TCP server listening on localhost:$port")

        acceptThread = Thread({
            acceptLoop()
        }, "TcpVideoSender-accept").apply {
            isDaemon = true
            start()
        }

        writerThread = Thread({
            writeLoop()
        }, "TcpVideoSender-writer").apply {
            isDaemon = true
            start()
        }
    }

    override fun send(rtpPacket: ByteArray) {
        if (!active.get() || !clientConnected) return

        // Wrap in VID0 framing (same format as UsbVideoSender)
        val vid0Packet = Protocol.buildVideoFrame(rtpPacket)

        // Non-blocking enqueue: drop oldest if full (real-time video)
        if (!sendQueue.offer(vid0Packet)) {
            sendQueue.poll()  // Drop oldest
            sendQueue.offer(vid0Packet)
        }
    }

    override fun isActive(): Boolean = active.get()

    override fun close() {
        if (!active.getAndSet(false)) return

        Log.i(TAG, "Closing TCP video sender. Stats: ${packetsSent.get()} packets, " +
                "${bytesSent.get() / 1024} KB")

        // Unblock the writer thread
        sendQueue.clear()

        // Close client connection
        closeClient()

        // Close server socket to unblock accept()
        try {
            serverSocket.close()
        } catch (e: Exception) {
            // Expected when closing
        }

        // Interrupt threads to ensure they exit
        acceptThread.interrupt()
        writerThread.interrupt()
    }

    private fun acceptLoop() {
        while (active.get()) {
            try {
                Log.i(TAG, "Waiting for client connection on port $port...")
                val socket = serverSocket.accept()

                socket.tcpNoDelay = true
                socket.sendBufferSize = 2 * 1024 * 1024

                synchronized(this) {
                    clientSocket = socket
                    clientOutput = socket.getOutputStream()
                    clientConnected = true
                }

                Log.i(TAG, "Client connected: ${socket.remoteSocketAddress}")

                // Flush any stale packets so the new client starts from a clean point
                sendQueue.clear()

                // Ask encoder for an immediate keyframe (IDR) to avoid showing stale/garbled frames
                try {
                    onClientConnected?.invoke()
                } catch (e: Exception) {
                    Log.w(TAG, "onClientConnected callback failed", e)
                }

                // Wait until the client disconnects (detected by writer thread)
                // or we're shut down
                while (active.get() && clientConnected) {
                    Thread.sleep(500)
                }

            } catch (e: Exception) {
                if (active.get()) {
                    Log.e(TAG, "Accept error", e)
                    Thread.sleep(1000)
                }
            }
        }
        Log.i(TAG, "Accept loop exited")
    }

    private fun writeLoop() {
        while (active.get()) {
            try {
                val packet = sendQueue.take() // Blocks until available

                val output = clientOutput
                if (output == null || !clientConnected) {
                    // No client connected, discard
                    continue
                }

                try {
                    output.write(packet)

                    val count = packetsSent.incrementAndGet()
                    bytesSent.addAndGet(packet.size.toLong())

                    if (count % STATS_INTERVAL == 0L) {
                        Log.d(TAG, "Sent $count packets, ${bytesSent.get() / 1024} KB")
                    }
                } catch (e: Exception) {
                    Log.w(TAG, "Write failed, client disconnected", e)
                    closeClient()
                }

            } catch (e: InterruptedException) {
                // Shutting down
                break
            }
        }
        Log.i(TAG, "Writer loop exited")
    }

    private fun closeClient() {
        synchronized(this) {
            clientConnected = false
            clientOutput = null
            try {
                clientSocket?.close()
            } catch (e: Exception) {
                // Ignore
            }
            clientSocket = null
        }
        // Drain queue so stale packets don't go to next client
        sendQueue.clear()
    }
}
