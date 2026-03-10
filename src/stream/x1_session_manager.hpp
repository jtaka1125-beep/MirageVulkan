// =============================================================================
// X1SessionManager — connects control TCP + canonical UDP, drives reconnect
// =============================================================================
#pragma once
#include "x1_protocol.hpp"
#include "canonical_receiver.hpp"
#include "canonical_frame.hpp"
#include "mirage_log.hpp"

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <chrono>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using ctrl_sock_t = SOCKET;
  constexpr ctrl_sock_t CTRL_INVALID = INVALID_SOCKET;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  using ctrl_sock_t = int;
  constexpr ctrl_sock_t CTRL_INVALID = -1;
  inline void closesocket(int s) { ::close(s); }
#endif

namespace mirage::x1 {

class X1SessionManager {
public:
    using FrameCallback = std::function<void(CanonicalFrame)>;

    explicit X1SessionManager() = default;
    ~X1SessionManager() { stop(); }

    void set_frame_callback(FrameCallback cb) {
        receiver_.set_callback(std::move(cb));
    }

    bool start(const std::string& device_ip) {
        if (running_.load()) return true;
        device_ip_ = device_ip;
        running_   = true;

        if (!receiver_.start(PORT_CANONICAL)) {
            MLOG_ERROR("x1_session", "failed to start UDP receiver");
            running_ = false;
            return false;
        }

        ctrl_thread_ = std::thread(&X1SessionManager::ctrl_loop, this);
        MLOG_INFO("x1_session", "started for %s", device_ip_.c_str());
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        close_ctrl();
        receiver_.stop();
        if (ctrl_thread_.joinable()) ctrl_thread_.join();
        MLOG_INFO("x1_session", "stopped");
    }

    bool send_idr()     { return send_cmd("IDR"); }
    bool send_pres_on() { return send_cmd("PRES_ON"); }
    bool send_pres_off(){ return send_cmd("PRES_OFF"); }
    bool send_bitrate(int kbps) {
        return send_cmd(std::string("BITRATE kbps=") + std::to_string(kbps));
    }

    // recv stats (drop diagnostics)
    using RecvStats = CanonicalReceiver::AssemblerStats;
    RecvStats get_recv_stats() const { return receiver_.get_stats(); }

    bool is_connected() const { return ctrl_connected_.load(); }
    bool running()      const { return running_.load(); }

private:
    std::string            device_ip_;
    std::atomic<bool>      running_{false};
    std::atomic<bool>      ctrl_connected_{false};

    ctrl_sock_t            ctrl_sock_ = CTRL_INVALID;
    std::mutex             ctrl_mutex_;

    CanonicalReceiver      receiver_;
    std::thread            ctrl_thread_;

    static constexpr int   RECONNECT_DELAY_MS = 2000;
    static constexpr int   PING_INTERVAL_MS   = 5000;

    void ctrl_loop() {
        while (running_.load()) {
            if (!connect_ctrl()) {
                MLOG_WARN("x1_session", "connect failed, retry in %dms", RECONNECT_DELAY_MS);
                sleep_ms(RECONNECT_DELAY_MS);
                continue;
            }
            if (!do_handshake()) {
                MLOG_WARN("x1_session", "handshake failed");
                close_ctrl();
                sleep_ms(RECONNECT_DELAY_MS);
                continue;
            }
            ctrl_connected_ = true;
            MLOG_INFO("x1_session", "connected and streaming");
            ping_loop();
            ctrl_connected_ = false;
            close_ctrl();
            if (running_.load()) {
                MLOG_WARN("x1_session", "disconnected, reconnect in %dms", RECONNECT_DELAY_MS);
                sleep_ms(RECONNECT_DELAY_MS);
            }
        }
    }

    bool connect_ctrl() {
        ctrl_sock_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == CTRL_INVALID) return false;
        set_timeout(s, 10000);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons((uint16_t)PORT_CONTROL);
        inet_pton(AF_INET, device_ip_.c_str(), &addr.sin_addr);
        if (::connect(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
            closesocket(s);
            return false;
        }
        std::lock_guard<std::mutex> lk(ctrl_mutex_);
        ctrl_sock_ = s;
        return true;
    }

    bool do_handshake() {
        if (!send_line("HELLO")) return false;
        std::string caps = recv_line();
        if (caps.find("CAPS") == std::string::npos) return false;
        MLOG_INFO("x1_session", "caps: %s", caps.c_str());
        if (!send_line("START")) return false;
        std::string ok = recv_line();
        return ok.find("OK") != std::string::npos;
    }

    void ping_loop() {
        while (running_.load() && ctrl_sock_ != CTRL_INVALID) {
            sleep_ms(PING_INTERVAL_MS);
            if (!send_line("PING")) break;
            std::string pong = recv_line();
            if (pong.find("PONG") == std::string::npos) break;
        }
    }

    bool send_cmd(const std::string& cmd) {
        std::lock_guard<std::mutex> lk(ctrl_mutex_);
        if (ctrl_sock_ == CTRL_INVALID) return false;
        return send_line_locked(cmd);
    }

    bool send_line(const std::string& line) {
        std::lock_guard<std::mutex> lk(ctrl_mutex_);
        return send_line_locked(line);
    }

    bool send_line_locked(const std::string& line) {
        std::string msg = line + "\n";
        int sent = ::send(ctrl_sock_, msg.c_str(), (int)msg.size(), 0);
        return sent == (int)msg.size();
    }

    std::string recv_line() {
        std::string result;
        char c = 0;
        while (ctrl_sock_ != CTRL_INVALID) {
            int n = ::recv(ctrl_sock_, &c, 1, 0);
            if (n <= 0) break;
            if (c == '\n') break;
            if (c != '\r') result += c;
        }
        return result;
    }

    void close_ctrl() {
        std::lock_guard<std::mutex> lk(ctrl_mutex_);
        if (ctrl_sock_ != CTRL_INVALID) {
            closesocket(ctrl_sock_);
            ctrl_sock_ = CTRL_INVALID;
        }
    }

    static void sleep_ms(int ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

    static void set_timeout(ctrl_sock_t s, int ms) {
#ifdef _WIN32
        DWORD tv = (DWORD)ms;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#else
        struct timeval tv{ ms/1000, (ms%1000)*1000 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
    }
};

} // namespace mirage::x1
