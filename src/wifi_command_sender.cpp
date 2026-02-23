#include "wifi_command_sender.hpp"
#include <system_error>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define WIFI_INVALID_SOCKET (~0ULL)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define closesocket close
#define WIFI_INVALID_SOCKET (-1)
#endif

#include "mirage_log.hpp"
#include "mirage_protocol.hpp"

using namespace mirage::protocol;

namespace gui {

WifiCommandSender::WifiCommandSender() = default;

WifiCommandSender::~WifiCommandSender() {
    stop();
}

void WifiCommandSender::setTarget(const std::string& ip, uint16_t port) {
    target_ip_ = ip;
    target_port_ = port;
}

bool WifiCommandSender::start() {
    if (running_.load()) return true;

    if (target_ip_.empty()) {
        MLOG_INFO("wificmd", "No target IP set");
        return false;
    }

    // Create UDP socket
    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == WIFI_INVALID_SOCKET) {
        MLOG_ERROR("wificmd", "Failed to create socket");
        return false;
    }

    // Set socket timeout for receive
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500000; // 500ms
    int setsock_result = setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    if (setsock_result != 0) {
        MLOG_ERROR("wificmd", "Warning: Failed to set socket timeout (err=%d)", setsock_result);
        // Continue anyway - timeout is not critical for functionality
    }

    running_.store(true);
    connected_.store(true);

    try {
        send_thread_ = std::thread(&WifiCommandSender::send_thread, this);
        recv_thread_ = std::thread(&WifiCommandSender::receive_thread, this);
    } catch (const std::system_error& e) {
        MLOG_ERROR("wificmd", "Failed to start threads: %s", e.what());
        running_.store(false);
        connected_.store(false);
        closesocket(socket_);
        socket_ = WIFI_INVALID_SOCKET;
        return false;
    }

    MLOG_INFO("wificmd", "Started (target: %s:%d)", target_ip_.c_str(), target_port_);
    return true;
}

void WifiCommandSender::stop() {
    running_.store(false);
    connected_.store(false);

    // ISSUE-16: shutdown socket before join so recvfrom() unblocks immediately
    if (socket_ != WIFI_INVALID_SOCKET) {
#ifdef _WIN32
        shutdown(socket_, SD_BOTH);
#else
        shutdown(socket_, SHUT_RDWR);
#endif
    }
    send_cv_.notify_one();  // ISSUE-17: also wake send_thread

    if (send_thread_.joinable()) send_thread_.join();
    if (recv_thread_.joinable()) recv_thread_.join();

    if (socket_ != WIFI_INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = WIFI_INVALID_SOCKET;
    }
}

void WifiCommandSender::send_thread() {
    MLOG_INFO("wificmd", "Send thread started");

    while (running_.load()) {
        std::vector<uint8_t> packet;

        {
            std::lock_guard<std::mutex> lock(queue_mtx_);
            if (!command_queue_.empty()) {
                packet = std::move(command_queue_.front());
                command_queue_.pop();
            }
        }

        if (!packet.empty()) {
            if (send_raw(packet.data(), packet.size())) {
                commands_sent_.fetch_add(1);
            }
        } else {
            // ISSUE-17: CV wait replaces 10ms spinlock
            std::unique_lock<std::mutex> cv_lk(send_cv_mtx_);
            send_cv_.wait_for(cv_lk, std::chrono::milliseconds(30));
        }
    }

    MLOG_INFO("wificmd", "Send thread ended");
}

void WifiCommandSender::receive_thread() {
    MLOG_INFO("wificmd", "Receive thread started");

    uint8_t buf[1024];
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);

    while (running_.load()) {
        int received = recvfrom(socket_, (char*)buf, sizeof(buf), 0,
                               (struct sockaddr*)&from_addr, &from_len);

        if (received >= (int)HEADER_SIZE) {
            // Parse ACK
            uint32_t magic = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
            uint8_t version = buf[4];
            uint8_t cmd = buf[5];
            uint32_t seq = buf[6] | (buf[7] << 8) | (buf[8] << 16) | (buf[9] << 24);

            if (magic == PROTOCOL_MAGIC && version == PROTOCOL_VERSION && cmd == CMD_ACK) {
                uint8_t status = (received >= (int)HEADER_SIZE + 5) ? buf[HEADER_SIZE + 4] : 0;
                acks_received_.fetch_add(1);

                // Calculate latency from ping
                {
                    std::lock_guard<std::mutex> lock(ping_mtx_);
                    auto it = pending_pings_.find(seq);
                    if (it != pending_pings_.end()) {
                        auto now = std::chrono::steady_clock::now();
                        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                            now - it->second
                        ).count();
                        latency_ms_.store(elapsed / 1000.0f);
                        pending_pings_.erase(it);
                    }
                }

                if (ack_callback_) {
                    ack_callback_(seq, status);
                }
            }
        }
    }

    MLOG_INFO("wificmd", "Receive thread ended");
}

bool WifiCommandSender::send_raw(const uint8_t* data, size_t len) {
    if (socket_ == WIFI_INVALID_SOCKET) return false;

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(target_port_);

    // Validate IP address before sending
    if (inet_pton(AF_INET, target_ip_.c_str(), &dest_addr.sin_addr) <= 0) {
        MLOG_ERROR("wificmd", "Invalid target IP address: %s", target_ip_.c_str());
        return false;
    }

    int sent = sendto(socket_, (const char*)data, (int)len, 0,
                     (struct sockaddr*)&dest_addr, sizeof(dest_addr));

    if (sent != (int)len) {
        MLOG_ERROR("wificmd", "Send error: sent %d of %zu", sent, len);
        return false;
    }

    return true;
}

std::vector<uint8_t> WifiCommandSender::build_packet(uint8_t cmd, const uint8_t* payload, size_t payload_len) {
    // Validate payload_len fits in protocol header (uint32_t)
    if (payload_len > UINT32_MAX) {
        MLOG_INFO("wificmd", "Payload too large: %zu bytes (max %u)", payload_len, UINT32_MAX);
        return {};
    }

    std::vector<uint8_t> packet(HEADER_SIZE + payload_len);

    uint32_t seq = next_seq_.fetch_add(1);

    // Header (little endian)
    packet[0] = PROTOCOL_MAGIC & 0xFF;
    packet[1] = (PROTOCOL_MAGIC >> 8) & 0xFF;
    packet[2] = (PROTOCOL_MAGIC >> 16) & 0xFF;
    packet[3] = (PROTOCOL_MAGIC >> 24) & 0xFF;
    packet[4] = PROTOCOL_VERSION;
    packet[5] = cmd;
    packet[6] = seq & 0xFF;
    packet[7] = (seq >> 8) & 0xFF;
    packet[8] = (seq >> 16) & 0xFF;
    packet[9] = (seq >> 24) & 0xFF;
    packet[10] = payload_len & 0xFF;
    packet[11] = (payload_len >> 8) & 0xFF;
    packet[12] = (payload_len >> 16) & 0xFF;
    packet[13] = (payload_len >> 24) & 0xFF;

    if (payload && payload_len > 0) {
        memcpy(packet.data() + HEADER_SIZE, payload, payload_len);
    }

    return packet;
}

uint32_t WifiCommandSender::send_ping() {
    auto packet = build_packet(CMD_PING, nullptr, 0);
    uint32_t seq = packet[6] | (packet[7] << 8) | (packet[8] << 16) | (packet[9] << 24);

    // Track ping time for latency measurement
    {
        std::lock_guard<std::mutex> lock(ping_mtx_);
        pending_pings_[seq] = std::chrono::steady_clock::now();
        // Clean old pings
        auto now = std::chrono::steady_clock::now();
        for (auto it = pending_pings_.begin(); it != pending_pings_.end(); ) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
            if (age > 5) {
                it = pending_pings_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::lock_guard<std::mutex> lock(queue_mtx_);
    command_queue_.push(std::move(packet));
    send_cv_.notify_one();  // ISSUE-17: wake send_thread
    return seq;
}

uint32_t WifiCommandSender::send_tap(int x, int y, int screen_w, int screen_h) {
    uint8_t payload[20];

    payload[0] = x & 0xFF;
    payload[1] = (x >> 8) & 0xFF;
    payload[2] = (x >> 16) & 0xFF;
    payload[3] = (x >> 24) & 0xFF;

    payload[4] = y & 0xFF;
    payload[5] = (y >> 8) & 0xFF;
    payload[6] = (y >> 16) & 0xFF;
    payload[7] = (y >> 24) & 0xFF;

    payload[8] = screen_w & 0xFF;
    payload[9] = (screen_w >> 8) & 0xFF;
    payload[10] = (screen_w >> 16) & 0xFF;
    payload[11] = (screen_w >> 24) & 0xFF;

    payload[12] = screen_h & 0xFF;
    payload[13] = (screen_h >> 8) & 0xFF;
    payload[14] = (screen_h >> 16) & 0xFF;
    payload[15] = (screen_h >> 24) & 0xFF;

    payload[16] = 0;
    payload[17] = 0;
    payload[18] = 0;
    payload[19] = 0;

    auto packet = build_packet(CMD_TAP, payload, sizeof(payload));
    uint32_t seq = packet[6] | (packet[7] << 8) | (packet[8] << 16) | (packet[9] << 24);

    std::lock_guard<std::mutex> lock(queue_mtx_);
    command_queue_.push(std::move(packet));
    send_cv_.notify_one();  // ISSUE-17

    MLOG_INFO("wificmd", "Queued TAP(%d, %d) seq=%u", x, y, seq);
    return seq;
}

uint32_t WifiCommandSender::send_swipe(int x1, int y1, int x2, int y2, int duration_ms) {
    uint8_t payload[20];

    payload[0] = x1 & 0xFF;
    payload[1] = (x1 >> 8) & 0xFF;
    payload[2] = (x1 >> 16) & 0xFF;
    payload[3] = (x1 >> 24) & 0xFF;

    payload[4] = y1 & 0xFF;
    payload[5] = (y1 >> 8) & 0xFF;
    payload[6] = (y1 >> 16) & 0xFF;
    payload[7] = (y1 >> 24) & 0xFF;

    payload[8] = x2 & 0xFF;
    payload[9] = (x2 >> 8) & 0xFF;
    payload[10] = (x2 >> 16) & 0xFF;
    payload[11] = (x2 >> 24) & 0xFF;

    payload[12] = y2 & 0xFF;
    payload[13] = (y2 >> 8) & 0xFF;
    payload[14] = (y2 >> 16) & 0xFF;
    payload[15] = (y2 >> 24) & 0xFF;

    payload[16] = duration_ms & 0xFF;
    payload[17] = (duration_ms >> 8) & 0xFF;
    payload[18] = (duration_ms >> 16) & 0xFF;
    payload[19] = (duration_ms >> 24) & 0xFF;

    auto packet = build_packet(CMD_SWIPE, payload, sizeof(payload));
    uint32_t seq = packet[6] | (packet[7] << 8) | (packet[8] << 16) | (packet[9] << 24);

    std::lock_guard<std::mutex> lock(queue_mtx_);
    command_queue_.push(std::move(packet));
    send_cv_.notify_one();  // ISSUE-17

    MLOG_INFO("wificmd", "Queued SWIPE(%d,%d)->(%d,%d) seq=%u", x1, y1, x2, y2, seq);
    return seq;
}

uint32_t WifiCommandSender::send_back() {
    uint8_t payload[4] = {0, 0, 0, 0};

    auto packet = build_packet(CMD_BACK, payload, sizeof(payload));
    uint32_t seq = packet[6] | (packet[7] << 8) | (packet[8] << 16) | (packet[9] << 24);

    std::lock_guard<std::mutex> lock(queue_mtx_);
    command_queue_.push(std::move(packet));
    send_cv_.notify_one();  // ISSUE-17

    MLOG_INFO("wificmd", "Queued BACK seq=%u", seq);
    return seq;
}

uint32_t WifiCommandSender::send_key(int keycode) {
    uint8_t payload[8];

    payload[0] = keycode & 0xFF;
    payload[1] = (keycode >> 8) & 0xFF;
    payload[2] = (keycode >> 16) & 0xFF;
    payload[3] = (keycode >> 24) & 0xFF;
    payload[4] = 0;
    payload[5] = 0;
    payload[6] = 0;
    payload[7] = 0;

    auto packet = build_packet(CMD_KEY, payload, sizeof(payload));
    uint32_t seq = packet[6] | (packet[7] << 8) | (packet[8] << 16) | (packet[9] << 24);

    std::lock_guard<std::mutex> lock(queue_mtx_);
    command_queue_.push(std::move(packet));
    send_cv_.notify_one();  // ISSUE-17

    MLOG_INFO("wificmd", "Queued KEY(%d) seq=%u", keycode, seq);
    return seq;
}

uint32_t WifiCommandSender::send_click_id(const std::string& resource_id) {
    std::vector<uint8_t> payload(2 + resource_id.size());
    uint16_t len = (uint16_t)resource_id.size();
    payload[0] = len & 0xFF;
    payload[1] = (len >> 8) & 0xFF;
    memcpy(payload.data() + 2, resource_id.c_str(), resource_id.size());

    auto packet = build_packet(CMD_CLICK_ID, payload.data(), payload.size());
    uint32_t seq = packet[6] | (packet[7] << 8) | (packet[8] << 16) | (packet[9] << 24);

    std::lock_guard<std::mutex> lock(queue_mtx_);
    command_queue_.push(std::move(packet));
    send_cv_.notify_one();  // ISSUE-17

    MLOG_INFO("wificmd", "Queued CLICK_ID(%s) seq=%u", resource_id.c_str(), seq);
    return seq;
}

uint32_t WifiCommandSender::send_click_text(const std::string& text) {
    std::vector<uint8_t> payload(2 + text.size());
    uint16_t len = (uint16_t)text.size();
    payload[0] = len & 0xFF;
    payload[1] = (len >> 8) & 0xFF;
    memcpy(payload.data() + 2, text.c_str(), text.size());

    auto packet = build_packet(CMD_CLICK_TEXT, payload.data(), payload.size());
    uint32_t seq = packet[6] | (packet[7] << 8) | (packet[8] << 16) | (packet[9] << 24);

    std::lock_guard<std::mutex> lock(queue_mtx_);
    command_queue_.push(std::move(packet));
    send_cv_.notify_one();  // ISSUE-17

    MLOG_INFO("wificmd", "Queued CLICK_TEXT(%s) seq=%u", text.c_str(), seq);
    return seq;
}

} // namespace gui
