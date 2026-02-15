package com.mirage.android.capture

/**
 * Interface for sending video packets.
 *
 * Implementations:
 * - UdpVideoSender: WiFi UDP streaming
 * - UsbVideoSender: USB AOA streaming with VID0 framing
 */
interface VideoSender {
    /**
     * Send an RTP packet.
     * @param rtpPacket Complete RTP packet including header (12+ bytes)
     */
    fun send(rtpPacket: ByteArray)

    /**
     * Check if sender is active and ready to send.
     */
    fun isActive(): Boolean

    /**
     * Close the sender and release resources.
     */
    fun close()
}
