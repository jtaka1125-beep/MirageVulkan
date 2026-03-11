// =============================================================================
// AiJpegReceiver 実装
// =============================================================================

#include "ai_jpeg_receiver.hpp"
#include "mirage_log.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#define closesocket close
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#endif

#include <cstring>

namespace mirage::ai {

AiJpegReceiver::~AiJpegReceiver() {
    stop();
}

bool AiJpegReceiver::start(const std::string& device_id, int port) {
    if (running_.load()) {
        MLOG_WARN("ai.jpeg", "Already running");
        return false;
    }

#ifdef _WIN32
    // Initialize Winsock (idempotent)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    device_id_ = device_id;
    port_ = port;

    // Create server socket
    server_sock_ = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock_ < 0) {
        MLOG_ERROR("ai.jpeg", "Failed to create socket");
        return false;
    }

    // Allow address reuse
    int opt = 1;
    setsockopt(server_sock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    // Bind
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(server_sock_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        MLOG_ERROR("ai.jpeg", "Failed to bind port %d", port);
        closesocket(server_sock_);
        server_sock_ = -1;
        return false;
    }

    // Listen
    if (listen(server_sock_, 1) == SOCKET_ERROR) {
        MLOG_ERROR("ai.jpeg", "Failed to listen");
        closesocket(server_sock_);
        server_sock_ = -1;
        return false;
    }

    running_.store(true);
    server_thread_ = std::thread(&AiJpegReceiver::serverThread, this);

    MLOG_INFO("ai.jpeg", "Started on port %d for device %s", port, device_id.c_str());
    return true;
}

void AiJpegReceiver::stop() {
    running_.store(false);

    if (server_sock_ != -1) {
        closesocket(server_sock_);
        server_sock_ = -1;
    }

    if (server_thread_.joinable()) {
        server_thread_.join();
    }

    MLOG_INFO("ai.jpeg", "Stopped. Frames: %llu, Bytes: %llu",
              (unsigned long long)frames_received_.load(),
              (unsigned long long)bytes_received_.load());
}

void AiJpegReceiver::serverThread() {
    MLOG_DEBUG("ai.jpeg", "Server thread started");

    while (running_.load()) {
        // Accept with timeout
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server_sock_, &fds);

        timeval tv{};
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ret = select(server_sock_ + 1, &fds, nullptr, nullptr, &tv);
        if (ret <= 0) continue;

        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        int client = static_cast<int>(accept(server_sock_, (sockaddr*)&client_addr, &addr_len));

        if (client < 0) {
            if (running_.load()) {
                MLOG_WARN("ai.jpeg", "Accept failed");
            }
            continue;
        }

        MLOG_INFO("ai.jpeg", "Client connected");

        // Handle client in same thread (single client expected)
        clientThread(client);

        MLOG_INFO("ai.jpeg", "Client disconnected");
    }

    MLOG_DEBUG("ai.jpeg", "Server thread exiting");
}

void AiJpegReceiver::clientThread(int client_sock) {
    // Frame format: [int32 len][int32 w][int32 h][int64 tsUs][bytes jpeg]
    // Note: Android uses big-endian (DataOutputStream)

    std::vector<uint8_t> jpeg_buf;

    while (running_.load()) {
        // Read header (20 bytes: 4+4+4+8)
        uint8_t header[20];
        if (!readExact(client_sock, header, 20)) {
            break;
        }

        // Parse header (big-endian)
        auto readInt32BE = [](const uint8_t* p) -> int32_t {
            return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
        };
        auto readInt64BE = [](const uint8_t* p) -> int64_t {
            return ((int64_t)p[0] << 56) | ((int64_t)p[1] << 48) |
                   ((int64_t)p[2] << 40) | ((int64_t)p[3] << 32) |
                   ((int64_t)p[4] << 24) | ((int64_t)p[5] << 16) |
                   ((int64_t)p[6] << 8)  | (int64_t)p[7];
        };

        int32_t len = readInt32BE(header);
        int32_t w   = readInt32BE(header + 4);
        int32_t h   = readInt32BE(header + 8);
        int64_t ts  = readInt64BE(header + 12);

        // Validate
        if (len <= 0 || len > 10 * 1024 * 1024) {  // Max 10MB
            MLOG_WARN("ai.jpeg", "Invalid frame length: %d", len);
            break;
        }
        if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
            MLOG_WARN("ai.jpeg", "Invalid dimensions: %dx%d", w, h);
            break;
        }

        // Read JPEG data
        jpeg_buf.resize(len);
        if (!readExact(client_sock, jpeg_buf.data(), len)) {
            break;
        }

        bytes_received_.fetch_add(20 + len);
        frames_received_.fetch_add(1);

        // Invoke callback
        if (callback_) {
            callback_(device_id_, jpeg_buf, w, h, ts);
        }
    }

    closesocket(client_sock);
}

bool AiJpegReceiver::readExact(int sock, void* buf, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t remaining = len;

    while (remaining > 0 && running_.load()) {
        // Use select with timeout for graceful shutdown
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);

        timeval tv{};
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ret = select(sock + 1, &fds, nullptr, nullptr, &tv);
        if (ret < 0) return false;
        if (ret == 0) continue;  // Timeout, check running_

        int n = recv(sock, (char*)p, static_cast<int>(remaining), 0);
        if (n <= 0) return false;

        p += n;
        remaining -= n;
    }

    return remaining == 0;
}

} // namespace mirage::ai
