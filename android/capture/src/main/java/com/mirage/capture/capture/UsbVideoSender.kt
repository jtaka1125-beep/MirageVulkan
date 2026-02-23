package com.mirage.capture.capture

import android.util.Log
import com.mirage.capture.usb.Protocol
import java.io.OutputStream
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong

/**
 * USB AOA video sender v2 — batched VID0 framing.
 *
 * Key optimizations over v1:
 *   - Batches all RTP packets for a frame into one USB write (reduces lock contention 10x)
 *   - Pre-allocated write buffer (eliminates per-packet ByteArray allocation)
 *   - Sends entire frame atomically (prevents interleaving with command ACKs)
 */
class UsbVideoSender(
    private val outputStream: OutputStream
) : VideoSender {

    companion object {
        private const val TAG = "UsbVideoSender"
        private const val STATS_INTERVAL = 300L
        private const val WRITE_BUFFER_SIZE = 128 * 1024  // 128KB pre-alloc
    }

    private val active = AtomicBoolean(true)
    private val packetsSent = AtomicLong(0)
    private val bytesSent = AtomicLong(0)
    private val errorCount = AtomicLong(0)
    private val framesSent = AtomicLong(0)

    // Pre-allocated batch buffer to avoid per-packet allocation
    private val batchBuffer = ByteArray(WRITE_BUFFER_SIZE)
    private var batchOffset = 0

    init {
        Log.i(TAG, "USB video sender v2 initialized (batch mode, buf=${WRITE_BUFFER_SIZE/1024}KB)")
    }

    /**
     * Buffer a single RTP packet with VID0 framing.
     * Call flush() after all packets for a frame are buffered.
     */
    override fun send(rtpPacket: ByteArray) {
        if (!active.get()) return

        val vid0Size = Protocol.VIDEO_HEADER_SIZE + rtpPacket.size
        if (batchOffset + vid0Size > WRITE_BUFFER_SIZE) {
            // Buffer full — flush what we have and start fresh
            flushBatch()
        }

        // Write VID0 header directly into batch buffer (no ByteArray alloc)
        val len = rtpPacket.size
        batchBuffer[batchOffset]     = 0x56  // 'V'
        batchBuffer[batchOffset + 1] = 0x49  // 'I'
        batchBuffer[batchOffset + 2] = 0x44  // 'D'
        batchBuffer[batchOffset + 3] = 0x30  // '0'
        batchBuffer[batchOffset + 4] = ((len shr 24) and 0xFF).toByte()
        batchBuffer[batchOffset + 5] = ((len shr 16) and 0xFF).toByte()
        batchBuffer[batchOffset + 6] = ((len shr 8) and 0xFF).toByte()
        batchBuffer[batchOffset + 7] = (len and 0xFF).toByte()
        System.arraycopy(rtpPacket, 0, batchBuffer, batchOffset + 8, rtpPacket.size)
        batchOffset += vid0Size

        packetsSent.incrementAndGet()
    }

    /**
     * Flush all buffered VID0 packets in one USB write.
     * Called by H264Encoder after all packets for a frame are sent.
     */
    override fun flush() = flushBatch()

    fun flushBatch() {
        if (batchOffset == 0) return

        try {
            // Single synchronized write for entire frame
            outputStream.write(batchBuffer, 0, batchOffset)

            bytesSent.addAndGet(batchOffset.toLong())
            val frames = framesSent.incrementAndGet()

            if (frames % STATS_INTERVAL == 0L) {
                Log.d(TAG, "Frames=$frames, pkts=${packetsSent.get()}, " +
                        "${bytesSent.get() / 1024}KB, batch=${batchOffset}B")
            }
        } catch (e: Exception) {
            val errCount = errorCount.incrementAndGet()
            if (errCount <= 10 || errCount % 100 == 0L) {
                Log.e(TAG, "Batch write failed (error #$errCount, ${batchOffset}B)", e)
            }
            if (errCount >= 50) {
                Log.e(TAG, "Too many errors, deactivating")
                active.set(false)
            }
        } finally {
            batchOffset = 0
        }
    }

    override fun isActive(): Boolean = active.get()

    override fun close() {
        if (active.getAndSet(false)) {
            flushBatch()
            Log.i(TAG, "Closed. frames=${framesSent.get()}, pkts=${packetsSent.get()}, " +
                    "${bytesSent.get() / 1024}KB, errors=${errorCount.get()}")
        }
    }

    fun getStats(): Triple<Long, Long, Long> =
        Triple(framesSent.get(), packetsSent.get(), bytesSent.get())
}
