#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <queue>

#ifdef USE_LIBUSB
#include <libusb-1.0/libusb.h>
#include "mirage_protocol.hpp"
#endif

namespace gui {

/**
 * USB AOA Command Sender
 * Sends control commands to Android device via USB AOA.
 *
 * Protocol (matches Android Protocol.kt):
 *   Header (14 bytes):
 *     magic:   4 bytes (0x4D495241 = "MIRA" LE)
 *     version: 1 byte  (1)
 *     cmd:     1 byte
 *     seq:     4 bytes
 *     len:     4 bytes (payload length)
 */
class UsbCommandSender {
public:

    using AckCallback = std::function<void(uint32_t seq, uint8_t status)>;
    using AudioCallback = std::function<void(const uint8_t* payload, size_t len, uint32_t timestamp)>;

    UsbCommandSender();
    ~UsbCommandSender();

    // Start/stop sender
    bool start();
    void stop();

    bool running() const { return running_.load(); }
    bool connected() const { return connected_.load(); }

    // Set callback for ACK responses
    void set_ack_callback(AckCallback cb) { ack_callback_ = cb; }
    void set_audio_callback(AudioCallback cb) { audio_callback_ = cb; }

    // Send commands (returns sequence number, 0 on error)
    uint32_t send_ping();
    uint32_t send_tap(int x, int y, int screen_w = 0, int screen_h = 0);
    uint32_t send_swipe(int x1, int y1, int x2, int y2, int duration_ms = 300);
    uint32_t send_back();
    uint32_t send_key(int keycode);
    uint32_t send_click_id(const std::string& resource_id);
    uint32_t send_click_text(const std::string& text);

    // Stats
    uint64_t commands_sent() const { return commands_sent_.load(); }
    uint64_t acks_received() const { return acks_received_.load(); }

private:
#ifdef USE_LIBUSB
    bool find_and_open_device();
    bool switch_device_to_aoa_mode();
    bool try_switch_android_to_aoa(libusb_device* dev);
    int get_aoa_protocol_version(libusb_device_handle* handle);
    bool send_aoa_string(libusb_device_handle* handle, uint16_t index, const char* str);
    void send_thread();
    void receive_thread();

    bool send_raw(const uint8_t* data, size_t len);
    std::vector<uint8_t> build_packet(uint8_t cmd, const uint8_t* payload, size_t payload_len);

    libusb_context* ctx_ = nullptr;
    libusb_device_handle* handle_ = nullptr;
    uint8_t ep_out_ = 0;
    uint8_t ep_in_ = 0;
#endif

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::thread send_thread_;
    std::thread recv_thread_;

    // Command queue
    std::mutex queue_mtx_;
    std::queue<std::vector<uint8_t>> command_queue_;

    // Sequence number
    std::atomic<uint32_t> next_seq_{1};

    // Callbacks
    AckCallback ack_callback_;
    AudioCallback audio_callback_;

    // Stats
    std::atomic<uint64_t> commands_sent_{0};
    std::atomic<uint64_t> acks_received_{0};
};

} // namespace gui
