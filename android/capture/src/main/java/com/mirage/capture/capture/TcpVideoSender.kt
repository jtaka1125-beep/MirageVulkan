package com.mirage.capture.capture

import android.util.Log
import com.mirage.capture.usb.Protocol
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
 * `adb forward tcp:PORT tcp:PORT` and receives VID0-framed RTP packets.
 *
 * v2: Auto-rebind ServerSocket on failure. Accept loop never exits
 * unless close() is called. Handles ServerSocket death gracefully.
 *
 * Thread model:
 * - Encoder thread calls send() which enqueues packets (non-blocking)
 * - Accept thread waits for client connections, rebinds on failure
 * - Writer thread dequeues and writes packets to the connected client
 */
class TcpVideoSender(
    private val port: Int = 50100,
    private val onClientConnected: (() -> Unit)? = null
) : VideoSender {

    companion object {
        private const val TAG = "TcpVideoSender"
        private const val QUEUE_CAPACITY = 256
        private const val STATS_INTERVAL = 100L
        private const val REBIND_MAX_ATTEMPTS = 5
        private const val REBIND_DELAY_MS = 2000L
    }

    private val active = AtomicBoolean(true)
    private val packetsSent = AtomicLong(0)
    private val bytesSent = AtomicLong(0)

    private val sendQueue = ArrayBlockingQueue<ByteArray>(QUEUE_CAPACITY)

    @Volatile
    private var clientConnected = false

    @Volatile
    private var serverSocket: ServerSocket? = null
    private var acceptThread: Thread? = null
    private var writerThread: Thread? = null

    @Volatile
    private var clientSocket: Socket? = null
    @Volatile
    private var clientOutput: OutputStream? = null

    init {
        val ss = rebindServerSocket()
        if (ss != null) {
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
        } else {
            Log.e(TAG, "Initial bind failed after $REBIND_MAX_ATTEMPTS attempts, degraded mode")
            active.set(false)
        }
    }

    /**
     * Bind or rebind ServerSocket with retry logic.
     * Handles TIME_WAIT from previous connections via reuseAddress.
     */
    private fun rebindServerSocket(): ServerSocket? {
        // Close existing socket first
        try { serverSocket?.close() } catch (_: Exception) {}
        serverSocket = null

        for (attempt in 1..REBIND_MAX_ATTEMPTS) {
            try {
                val ss = ServerSocket()
                ss.reuseAddress = true
                ss.bind(java.net.InetSocketAddress(InetAddress.getByName("0.0.0.0"), port), 1)
                serverSocket = ss
                Log.i(TAG, "ServerSocket bound on port $port (attempt $attempt)")
                return ss
            } catch (e: Exception) {
                Log.w(TAG, "bind() attempt $attempt/$REBIND_MAX_ATTEMPTS failed: ${e.message}")
                if (attempt < REBIND_MAX_ATTEMPTS) {
                    Thread.sleep(REBIND_DELAY_MS)
                }
            }
        }
        return null
    }

    override fun send(rtpPacket: ByteArray) {
        if (!active.get() || !clientConnected) return

        val vid0Packet = Protocol.buildVideoFrame(rtpPacket)

        if (!sendQueue.offer(vid0Packet)) {
            sendQueue.poll()
            sendQueue.offer(vid0Packet)
        }
    }

    override fun isActive(): Boolean = active.get()

    override fun close() {
        if (!active.getAndSet(false)) return

        Log.i(TAG, "Closing TCP video sender. Stats: ${packetsSent.get()} packets, " +
                "${bytesSent.get() / 1024} KB")

        sendQueue.clear()
        closeClient()

        try { serverSocket?.close() } catch (_: Exception) {}
        serverSocket = null

        acceptThread?.interrupt()
        writerThread?.interrupt()
    }

    private fun acceptLoop() {
        while (active.get()) {
            try {
                // Ensure ServerSocket is alive, rebind if needed
                var ss = serverSocket
                if (ss == null || ss.isClosed) {
                    Log.w(TAG, "ServerSocket dead, attempting rebind...")
                    ss = rebindServerSocket()
                    if (ss == null) {
                        Log.e(TAG, "Rebind failed, retrying in 3s")
                        Thread.sleep(3000)
                        continue
                    }
                }

                Log.i(TAG, "Waiting for client connection on port $port...")
                val socket = ss.accept()

                socket.tcpNoDelay = true
                socket.sendBufferSize = 2 * 1024 * 1024

                // Close previous client if any
                closeClient()

                synchronized(this) {
                    clientSocket = socket
                    clientOutput = socket.getOutputStream()
                    clientConnected = true
                }

                Log.i(TAG, "Client connected: ${socket.remoteSocketAddress}")
                onClientConnected?.invoke()

                // Wait until client disconnects (detected by writer thread)
                while (active.get() && clientConnected) {
                    Thread.sleep(500)
                }
                Log.i(TAG, "Client disconnected, waiting for next connection")

            } catch (e: Exception) {
                if (active.get()) {
                    Log.e(TAG, "Accept error, will rebind: ${e.message}")
                    // Force ServerSocket recreation on next loop iteration
                    try { serverSocket?.close() } catch (_: Exception) {}
                    serverSocket = null
                    Thread.sleep(2000)
                }
            }
        }
        Log.i(TAG, "Accept loop exited")
    }

    private fun writeLoop() {
        while (active.get()) {
            try {
                val packet = sendQueue.take()

                val output = clientOutput
                if (output == null || !clientConnected) {
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
                    Log.w(TAG, "Write failed, client disconnected: ${e.message}")
                    closeClient()
                }

            } catch (e: InterruptedException) {
                break
            }
        }
        Log.i(TAG, "Writer loop exited")
    }

    private fun closeClient() {
        synchronized(this) {
            clientConnected = false
            clientOutput = null
            try { clientSocket?.close() } catch (_: Exception) {}
            clientSocket = null
        }
        sendQueue.clear()
    }
}
