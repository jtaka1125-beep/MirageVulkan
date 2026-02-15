#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <queue>
#include <map>
#include <chrono>
#include "mirage_protocol.hpp"

namespace gui {

/**
 * WiFi Command Sender
 * Sends control commands to Android device via UDP.
 *
 * Protocol (same as USB, matches Android Protocol.kt):
 *   Header (14 bytes):
 *     magic:   4 bytes (0x4D495241 = "MIRA" LE)
 *     version: 1 byte  (1)
 *     cmd:     1 byte
 *     seq:     4 bytes
 *     len:     4 bytes (payload length)
 */
class WifiCommandSender {
public:

    using AckCallback = std::function<void(uint32_t seq, uint8_t status)>;

    WifiCommandSender();
    ~WifiCommandSender();

    // Set target Android device
    void setTarget(const std::string& ip, uint16_t port = 60001);

    // Start/stop sender
    bool start();
    void stop();

    bool running() const { return running_.load(); }
    bool connected() const { return connected_.load(); }

    // Set callback for ACK responses
    void set_ack_callback(AckCallback cb) { ack_callback_ = cb; }

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

    // Latency measurement
    float latency_ms() const { return latency_ms_.load(); }

private:
    void send_thread();
    void receive_thread();

    bool send_raw(const uint8_t* data, size_t len);
    std::vector<uint8_t> build_packet(uint8_t cmd, const uint8_t* payload, size_t payload_len);

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::thread send_thread_;
    std::thread recv_thread_;

    // Target address
    std::string target_ip_;
    uint16_t target_port_ = 60001;

    // Socket
#ifdef _WIN32
    unsigned long long socket_ = ~0ULL;  // INVALID_SOCKET
#else
    int socket_ = -1;
#endif

    // Command queue
    std::mutex queue_mtx_;
    std::queue<std::vector<uint8_t>> command_queue_;

    // Sequence number
    std::atomic<uint32_t> next_seq_{1};

    // Callback
    AckCallback ack_callback_;

    // Stats
    std::atomic<uint64_t> commands_sent_{0};
    std::atomic<uint64_t> acks_received_{0};
    std::atomic<float> latency_ms_{0.0f};

    // Ping tracking for latency measurement
    std::mutex ping_mtx_;
    std::map<uint32_t, std::chrono::steady_clock::time_point> pending_pings_;
};

} // namespace gui
