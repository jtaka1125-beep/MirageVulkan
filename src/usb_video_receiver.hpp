#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <cstring>

#ifdef USE_LIBUSB
#include <libusb-1.0/libusb.h>
#include "mirage_protocol.hpp"
#endif

namespace gui {

/**
 * USB AOA Video Receiver v3  Ehigh-throughput optimized
 *
 * Target: 30+ fps (baseline: 15fps)
 *
 * Optimizations:
 *   1. Ring buffer w/ memcpy (O(1) vs deque O(n))
 *   2. Double-buffered async libusb (zero wait between transfers)
 *   3. SPS/PPS passthrough during flush (instant decoder init)
 *   4. memchr-based magic scanning
 *   5. 20ms poll, 16ms flush
 */
class UsbVideoReceiver {
public:
    static constexpr uint32_t USB_VIDEO_MAGIC = 0x56494430;

    static constexpr size_t USB_BUFFER_SIZE = 131072;
    static constexpr size_t RING_BUFFER_SIZE = 1024 * 1024;
    static constexpr uint32_t MIN_PACKET_LEN = 12;
    static constexpr uint32_t MAX_PACKET_LEN = 65535;
    static constexpr int USB_TIMEOUT_MS = 20;
    static constexpr int FLUSH_PERIOD_MS = 16;
    static constexpr int NUM_TRANSFERS = 8;

    using RtpCallback = std::function<void(const uint8_t* data, size_t len)>;

    UsbVideoReceiver();
    ~UsbVideoReceiver();

    void set_rtp_callback(RtpCallback cb) { rtp_callback_ = cb; }
    void set_target_serial(const std::string& serial) { target_serial_ = serial; }
    void set_device_index(int idx) { device_index_ = idx; }

    bool start();
    void stop();

    bool running() const { return running_.load(); }
    bool connected() const { return connected_.load(); }
    uint64_t packets_received() const { return packets_received_.load(); }
    uint64_t bytes_received() const { return bytes_received_.load(); }

private:
#ifdef USE_LIBUSB
    void receive_thread();
    bool find_and_open_device();
    void process_ring();
    void process_ring_flush(uint64_t& sps_pps_count);

    struct TransferSlot {
        libusb_transfer* transfer = nullptr;
        uint8_t buffer[USB_BUFFER_SIZE];
        UsbVideoReceiver* owner = nullptr;
    };
    TransferSlot transfers_[NUM_TRANSFERS];
    static void LIBUSB_CALL transfer_callback(libusb_transfer* transfer);
    bool setup_async_transfers();
    void cancel_async_transfers();

    libusb_context* ctx_ = nullptr;
    libusb_device_handle* handle_ = nullptr;
    uint8_t ep_in_ = 0;
#endif

    std::vector<uint8_t> ring_;
    size_t ring_head_ = 0;
    size_t ring_tail_ = 0;
    std::mutex ring_mtx_;

    size_t ring_available() const {
        return (ring_head_ >= ring_tail_) ? (ring_head_ - ring_tail_)
                                          : (RING_BUFFER_SIZE - ring_tail_ + ring_head_);
    }
    void ring_write(const uint8_t* data, size_t len);
    void ring_read(uint8_t* dst, size_t len);
    void ring_skip(size_t len);
    uint8_t ring_peek(size_t offset) const;
    size_t ring_contiguous_from_tail() const;
    bool ring_find_magic(size_t& offset_out) const;

    static bool is_sps_pps_rtp(const uint8_t* data, size_t len);

    std::string target_serial_;
    int device_index_ = -1;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::thread thread_;
    RtpCallback rtp_callback_;

    std::atomic<uint64_t> packets_received_{0};
    std::atomic<uint64_t> bytes_received_{0};
    uint64_t drop_count_ = 0;
    uint64_t sync_errors_ = 0;
};

} // namespace gui
