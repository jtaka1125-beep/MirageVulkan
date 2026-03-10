// =============================================================================
// MonitorReceiver — Phase C: UDP socket → MonitorAssembler
//
// Binds UDP :50202, receives MFRM/H.264 datagrams,
// feeds MonitorAssembler, and delivers complete MonitorFrame via callback.
//
// The callback is invoked from the internal recv thread.
// =============================================================================
#pragma once
#include "x1_protocol.hpp"
#include "monitor_frame.hpp"
#include "monitor_assembler.hpp"
#include "mirage_log.hpp"

#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using mon_sock_t = SOCKET;
  constexpr mon_sock_t MON_INVALID_SOCK = INVALID_SOCKET;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  using mon_sock_t = int;
  constexpr mon_sock_t MON_INVALID_SOCK = -1;
  inline void closesocket(int s) { close(s); }
#endif

namespace mirage::x1 {

/**
 * MonitorReceiver
 *
 * Listens on UDP :50202 (PORT_PRESENTATION) for H.264 NAL fragments.
 * Delivers assembled MonitorFrame objects via callback.
 *
 * Stats logged every 300 frames (~10s @ 30fps).
 */
class MonitorReceiver {
public:
    using FrameCallback = std::function<void(MonitorFrame)>;

    MonitorReceiver() = default;
    ~MonitorReceiver() { stop(); }

    void set_callback(FrameCallback cb) {
        std::lock_guard<std::mutex> lk(cb_mutex_);
        callback_ = std::move(cb);
        assembler_.set_callback([this](MonitorFrame f){
            std::lock_guard<std::mutex> lk2(cb_mutex_);
            if (callback_) callback_(std::move(f));
        });
    }

    bool start(int port = PORT_PRESENTATION) {
        if (running_.load()) return true;

#ifdef _WIN32
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2,2), &wsa);
#endif
        sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ == MON_INVALID_SOCK) {
            MLOG_ERROR("monitor_recv", "socket() failed");
            return false;
        }

        // SO_RCVBUF: 8MB — H.264 frames can be larger than JPEG
        int rcvbuf = 8 * 1024 * 1024;
        setsockopt(sock_, SOL_SOCKET, SO_RCVBUF,
                   (const char*)&rcvbuf, sizeof(rcvbuf));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons((uint16_t)port);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(sock_, (sockaddr*)&addr, sizeof(addr)) != 0) {
            MLOG_ERROR("monitor_recv", "bind() failed port=%d", port);
            closesocket(sock_);
            sock_ = MON_INVALID_SOCK;
            return false;
        }

        running_ = true;
        recv_thread_  = std::thread(&MonitorReceiver::recv_loop,  this);
        stale_thread_ = std::thread(&MonitorReceiver::stale_loop, this);
        MLOG_INFO("monitor_recv", "listening UDP :%d (Monitor/H.264)", port);
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (sock_ != MON_INVALID_SOCK) {
            closesocket(sock_);
            sock_ = MON_INVALID_SOCK;
        }
        if (recv_thread_.joinable())  recv_thread_.join();
        if (stale_thread_.joinable()) stale_thread_.join();
        assembler_.log_stats("final");
        MLOG_INFO("monitor_recv", "stopped.");
    }

    bool running() const { return running_.load(); }

    using AssemblerStats = MonitorAssembler::Stats;
    AssemblerStats get_stats() const { return assembler_.stats(); }

private:
    static constexpr size_t BUF_SIZE = HEADER_SIZE + MTU_PAYLOAD + 64;

    mon_sock_t  sock_    = MON_INVALID_SOCK;
    std::atomic<bool> running_{false};
    std::thread recv_thread_;
    std::thread stale_thread_;

    MonitorAssembler assembler_;

    mutable std::mutex cb_mutex_;
    FrameCallback      callback_;

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

            if ((size_t)n < HEADER_SIZE) continue;

            auto hdr = parse_header(buf.data(), (size_t)n);
            if (!hdr) continue;

            // Accept LANE_PRESENTATION only
            if (hdr->lane != LANE_PRESENTATION) continue;

            const uint8_t* payload     = buf.data() + HEADER_SIZE;
            size_t         payload_len = (size_t)n - HEADER_SIZE;

            assembler_.feed(*hdr, payload, payload_len);
            ++pkts;

            // Log stats every 300 pkts (~10s @ 30fps)
            if (pkts % 300 == 0) {
                assembler_.log_stats("monitor");
            }
        }
        MLOG_INFO("monitor_recv", "recv loop exited. pkts=%llu", (unsigned long long)pkts);
    }

    // ── Stale flush loop (every 100ms) ─────────────────────────────────────

    void stale_loop() {
        while (running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            assembler_.flush_stale(200);
        }
    }
};

} // namespace mirage::x1
