package com.mirage.android.capture

import android.util.Log
import com.mirage.android.usb.Protocol
import java.io.OutputStream
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong

/**
 * USB AOA video sender with VID0 framing.
 *
 * Sends H.264 RTP packets over USB AOA using VID0 protocol:
 * [VID0(4 bytes)][length(4 bytes)][RTP data]
 *
 * Thread-safety: Uses synchronized OutputStream shared with command sending.
 * The outputStream should be a SynchronizedOutputStream from AccessoryIoService.
 *
 * @param outputStream Synchronized output stream from AccessoryIoService
 */
class UsbVideoSender(
    private val outputStream: OutputStream
) : VideoSender {

    companion object {
        private const val TAG = "UsbVideoSender"
        private const val MAX_RTP_SIZE = 1500  // Typical MTU
        private const val STATS_INTERVAL = 100L  // Log every N packets
    }

    private val active = AtomicBoolean(true)
    private val packetsSent = AtomicLong(0)
    private val bytesSent = AtomicLong(0)
    private val errorCount = AtomicLong(0)

    init {
        Log.i(TAG, "USB video sender initialized")
    }

    override fun send(rtpPacket: ByteArray) {
        if (!active.get()) return

        if (rtpPacket.size > MAX_RTP_SIZE) {
            Log.w(TAG, "RTP packet exceeds typical MTU: ${rtpPacket.size} bytes")
        }

        try {
            // Wrap RTP packet in VID0 framing
            val vid0Packet = Protocol.buildVideoFrame(rtpPacket)

            // Write to synchronized output stream
            outputStream.write(vid0Packet)

            // Track statistics
            val count = packetsSent.incrementAndGet()
            bytesSent.addAndGet(vid0Packet.size.toLong())

            // Periodic stats logging
            if (count % STATS_INTERVAL == 0L) {
                Log.d(TAG, "Sent $count packets, ${bytesSent.get() / 1024} KB")
            }

        } catch (e: Exception) {
            val errCount = errorCount.incrementAndGet()
            if (errCount <= 10 || errCount % 100 == 0L) {
                Log.e(TAG, "Failed to send video frame (error #$errCount)", e)
            }

            // Deactivate after too many errors
            if (errCount >= 50) {
                Log.e(TAG, "Too many errors, deactivating USB video sender")
                active.set(false)
            }
        }
    }

    override fun isActive(): Boolean = active.get()

    override fun close() {
        if (active.getAndSet(false)) {
            Log.i(TAG, "USB video sender closed. Stats: ${packetsSent.get()} packets, " +
                    "${bytesSent.get() / 1024} KB, ${errorCount.get()} errors")
        }
        // Note: Don't close the outputStream here - it's shared with command sending
    }

    /**
     * Get current statistics.
     * @return Pair of (packets sent, bytes sent)
     */
    fun getStats(): Pair<Long, Long> = Pair(packetsSent.get(), bytesSent.get())
}
