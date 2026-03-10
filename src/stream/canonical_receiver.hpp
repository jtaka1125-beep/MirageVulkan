// =============================================================================
// X1 Canonical Receiver — UDP socket → CanonicalFrameAssembler
// =============================================================================
#pragma once
#include "x1_protocol.hpp"
#include "canonical_frame.hpp"
#include "canonical_frame_assembler.hpp"
#include "mirage_log.hpp"

#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <memory>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using sock_t = SOCKET;
  constexpr sock_t INVALID_SOCK = INVALID_SOCKET;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  using sock_t = int;
  constexpr sock_t INVALID_SOCK = -1;
  inline void closesocket(int s) { close(s); }
#endif

namespace mirage::x1 {

/**
 * CanonicalReceiver
 *
 * Binds UDP :50201, receives MFRM datagrams, feeds the assembler,
 * and delivers complete CanonicalFrame objects via callback.
 *
 * The callback is invoked from the internal recv thread.
 * Caller must ensure the callback is thread-safe.
 */
class CanonicalReceiver {
public:
    using FrameCallback = std::function<void(CanonicalFrame)>;

    CanonicalReceiver() = default;
    ~CanonicalReceiver() { stop(); }

    void set_callback(FrameCallback cb) {
        std::lock_guard<std::mutex> lk(cb_mutex_);
        callback_ = std::move(cb);
        assembler_.set_callback([this](CanonicalFrame f){
            std::lock_guard<std::mutex> lk2(cb_mutex_);
            if (callback_) callback_(std::move(f));
        });
    }

    bool start(int port = PORT_CANONICAL) {
        if (running_.load()) return true;

#ifdef _WIN32
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2,2), &wsa);
#endif
        sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ == INVALID_SOCK) {
            MLOG_ERROR("x1_recv", "socket() failed");
            return false;
        }

        // SO_RCVBUF: 4MB to absorb burst
        int rcvbuf = 4 * 1024 * 1024;
        setsockopt(sock_, SOL_SOCKET, SO_RCVBUF,
                   (const char*)&rcvbuf, sizeof(rcvbuf));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons((uint16_t)port);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(sock_, (sockaddr*)&addr, sizeof(addr)) != 0) {
            MLOG_ERROR("x1_recv", "bind() failed port=%d", port);
            closesocket(sock_);
            sock_ = INVALID_SOCK;
            return false;
        }

        running_ = true;
        recv_thread_ = std::thread(&CanonicalReceiver::recv_loop, this);
        stale_thread_ = std::thread(&CanonicalReceiver::stale_loop, this);
        MLOG_INFO("x1_recv", "listening UDP :%d", port);
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (sock_ != INVALID_SOCK) {
            closesocket(sock_);
            sock_ = INVALID_SOCK;
        }
        if (recv_thread_.joinable())  recv_thread_.join();
        if (stale_thread_.joinable()) stale_thread_.join();
        MLOG_INFO("x1_recv", "stopped. frames=%llu dropped_old=%llu evicted=%llu",
                  assembler_.stats().delivered,
                  assembler_.stats().dropped_old,
                  assembler_.stats().incomplete_frames_evicted);
    }

    bool running() const { return running_.load(); }

    // assembler stats 公開 (drop診断用)
    using AssemblerStats = CanonicalFrameAssembler::Stats;
    AssemblerStats get_stats() const { return assembler_.stats(); }

    // Latest complete frame (thread-safe snapshot)
    CanonicalFrame latest_frame() const {
        std::lock_guard<std::mutex> lk(latest_mutex_);
        return latest_;
    }

private:
    static constexpr size_t BUF_SIZE = HEADER_SIZE + MTU_PAYLOAD + 64;

    sock_t    sock_    = INVALID_SOCK;
    std::atomic<bool> running_{false};
    std::thread recv_thread_;
    std::thread stale_thread_;

    CanonicalFrameAssembler assembler_;

    mutable std::mutex cb_mutex_;
    FrameCallback      callback_;

    mutable std::mutex latest_mutex_;
    CanonicalFrame     latest_;

    // ── Recv loop ──────────────────────────────────────────────────────────

    void recv_loop() {
        std::vector<uint8_t> buf(BUF_SIZE);
        uint64_t pkts = 0;

        while (running_.load()) {
            sockaddr_in from{};
            socklen_t   from_len = sizeof(from);
            int n = recvfrom(sock_, (char*)buf.data(), (int)buf.size(), 0,
                             (sockaddr*)&from, &from_len);
            if (n <= 0) break;

            auto hdr = parse_header(buf.data(), (size_t)n);
            if (!hdr || !hdr->is_canonical()) continue;

            const uint8_t* payload     = buf.data() + HEADER_SIZE;
            size_t         payload_len = (size_t)n - HEADER_SIZE;

            assembler_.feed(*hdr, payload, payload_len);

            if (++pkts % 150 == 0) {
                MLOG_DEBUG("x1_recv", "pkts=%llu delivered=%llu old=%llu stale=%llu",
                           pkts,
                           assembler_.stats().delivered,
                           assembler_.stats().dropped_old,
                           assembler_.stats().incomplete_frames_evicted);
            }
        }
        MLOG_INFO("x1_recv", "recv loop exited");
    }

    // ── Stale flush loop (every 100ms) ─────────────────────────────────────

    void stale_loop() {
        while (running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            assembler_.flush_stale(150);
        }
    }
};

} // namespace mirage::x1
