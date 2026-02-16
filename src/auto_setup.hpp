// =============================================================================
// MirageSystem v2 - Auto Setup (scrcpy-server edition)
// =============================================================================
// Uses scrcpy-server for screen mirroring - no MediaProjection dialog needed.
// Flow: scrcpy-server (app_process) -> Unix socket -> ADB forward -> TCP -> UDP
// =============================================================================
#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <cstdio>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#include <windows.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

#include "mirage_log.hpp"

namespace mirage {

enum class SetupStatus {
    PENDING,
    IN_PROGRESS,
    COMPLETED,
    SKIPPED,
    FAILED
};

struct SetupStepResult {
    SetupStatus status = SetupStatus::PENDING;
    std::string message;
};

struct AutoSetupResult {
    bool success = false;
    std::string error;
    std::string summary() const { return success ? "OK" : error; }
};

class AutoSetup {
public:
    using ProgressCallback = std::function<void(const std::string& step, int progress)>;
    using AdbExecutor = std::function<std::string(const std::string& cmd)>;

    void setProgressCallback(ProgressCallback cb) { progress_callback_ = std::move(cb); }
    void set_progress_callback(ProgressCallback cb) { progress_callback_ = std::move(cb); }
    void set_adb_executor(AdbExecutor cb) { adb_executor_ = std::move(cb); }

    ~AutoSetup() { stop_bridge(); }

    AutoSetupResult run(const std::string& device_id, void* adb_manager) {
        (void)device_id; (void)adb_manager;
        AutoSetupResult r; r.success = true; return r;
    }
    AutoSetupResult run(bool full_setup) {
        (void)full_setup;
        AutoSetupResult r; r.success = true; return r;
    }

    // =========================================================================
    // Start screen capture via scrcpy-server
    // host: PC IP for UDP target (not used for scrcpy, kept for API compat)
    // port: UDP port that MirrorReceiver is listening on
    // =========================================================================
    SetupStepResult start_screen_capture(const std::string& host, int port) {
        (void)host; // scrcpy streams to TCP, bridge sends to UDP locally
        SetupStepResult result;

        if (progress_callback_)
            progress_callback_("Starting scrcpy-server...", 10);

        if (!adb_executor_) {
            result.status = SetupStatus::FAILED;
            result.message = "No ADB executor";
            return result;
        }

        // Generate unique SCID and TCP port
        scid_ = 0x20000000 + (rand() & 0x1FFFFFFF);
        char scid_str[16];
        snprintf(scid_str, sizeof(scid_str), "%08x", scid_);
        tcp_port_ = 27183 + (port % 100); // Unique per device
        udp_port_ = port;

        MLOG_INFO("adb", "scrcpy: scid=%s tcp=%d udp=%d", scid_str, tcp_port_, udp_port_);

        // 1. Push scrcpy-server (idempotent, fast if already there)
        adb_executor_("push tools/scrcpy-server-v3.3.4 /data/local/tmp/scrcpy-server.jar");

        // 2. Kill any existing scrcpy on this device
        adb_executor_("shell pkill -f scrcpy 2>/dev/null");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // 3. ADB forward
        std::string fwd = "forward tcp:" + std::to_string(tcp_port_) +
                          " localabstract:scrcpy_" + scid_str;
        adb_executor_(fwd);

        // 4. Start scrcpy-server in background
        std::string shell_cmd = "shell CLASSPATH=/data/local/tmp/scrcpy-server.jar"
            " app_process / com.genymobile.scrcpy.Server 3.3.4"
            " tunnel_forward=true audio=false control=false"
            " raw_stream=true max_size=720 video_bit_rate=2000000"
            " max_fps=30 cleanup=false scid=" + std::string(scid_str);

        // Start in background thread (blocking ADB shell)
        server_running_ = true;
        server_thread_ = std::thread([this, shell_cmd]() {
            if (adb_executor_) {
                std::string out = adb_executor_(shell_cmd);
                MLOG_INFO("adb", "scrcpy server exited: %s", out.c_str());
            }
            server_running_ = false;
        });
        server_thread_.detach();

        // 5. Wait for server to start, then launch TCP->UDP bridge
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));

        if (progress_callback_)
            progress_callback_("Connecting to scrcpy stream...", 50);

        // Start bridge thread
        bridge_running_ = true;
        bridge_thread_ = std::thread(&AutoSetup::bridge_loop, this);

        result.status = SetupStatus::COMPLETED;
        result.message = "scrcpy started";
        return result;
    }

    // No dialog needed for scrcpy - NOP
    SetupStepResult approve_screen_share_dialog() {
        SetupStepResult result;
        result.status = SetupStatus::COMPLETED;
        result.message = "scrcpy: no dialog needed";
        if (progress_callback_)
            progress_callback_("No permission dialog needed (scrcpy)", 75);
        return result;
    }

    SetupStepResult complete_and_verify() {
        SetupStepResult result;
        if (bridge_connected_) {
            result.status = SetupStatus::COMPLETED;
            result.message = "";
        } else {
            // Give it a moment
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            result.status = bridge_connected_ ? SetupStatus::COMPLETED : SetupStatus::FAILED;
            result.message = bridge_connected_ ? "" : "Bridge not connected";
        }
        if (progress_callback_)
            progress_callback_("Setup complete", 100);
        return result;
    }

private:
    ProgressCallback progress_callback_;
    AdbExecutor adb_executor_;

    // scrcpy state
    uint32_t scid_ = 0;
    int tcp_port_ = 0;
    int udp_port_ = 0;
    std::atomic<bool> server_running_{false};
    std::atomic<bool> bridge_running_{false};
    std::atomic<bool> bridge_connected_{false};
    std::thread server_thread_;
    std::thread bridge_thread_;

    void stop_bridge() {
        bridge_running_ = false;
        if (bridge_thread_.joinable())
            bridge_thread_.join();
    }

    // TCP (scrcpy) -> UDP (MirrorReceiver) bridge
    void bridge_loop() {
        MLOG_INFO("adb", "Bridge thread starting: TCP:%d -> UDP:%d", tcp_port_, udp_port_);

        // Connect TCP to scrcpy-server
        SOCKET tcp_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (tcp_sock == INVALID_SOCKET) {
            MLOG_ERROR("adb", "Failed to create TCP socket");
            return;
        }

        // TCP_NODELAY for low latency
        int nodelay = 1;
        setsockopt(tcp_sock, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(nodelay));

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(static_cast<u_short>(tcp_port_));
        inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

        // Retry connect
        bool connected = false;
        for (int i = 0; i < 20 && bridge_running_; i++) {
            if (connect(tcp_sock, (sockaddr*)&server_addr, sizeof(server_addr)) == 0) {
                connected = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        if (!connected) {
            MLOG_ERROR("adb", "Bridge: TCP connect failed after retries");
            closesocket(tcp_sock);
            return;
        }

        MLOG_INFO("adb", "Bridge: TCP connected to scrcpy on port %d", tcp_port_);
        bridge_connected_ = true;

        // Create UDP sender
        SOCKET udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in udp_dest{};
        udp_dest.sin_family = AF_INET;
        udp_dest.sin_port = htons(static_cast<u_short>(udp_port_));
        inet_pton(AF_INET, "127.0.0.1", &udp_dest.sin_addr);

        // Bridge: read H.264 from TCP, send to UDP
        char buf[65536];
        long long total = 0;
        auto start = std::chrono::steady_clock::now();

        while (bridge_running_) {
            int n = recv(tcp_sock, buf, sizeof(buf), 0);
            if (n <= 0) {
                MLOG_WARN("adb", "Bridge: TCP recv returned %d", n);
                break;
            }
            // Forward raw H.264 to UDP (MirrorReceiver parses NAL units)
            // Send in chunks ≤ 1400 bytes for UDP MTU
            for (int offset = 0; offset < n; offset += 1400) {
                int chunk = std::min(n - offset, 1400);
                sendto(udp_sock, buf + offset, chunk, 0,
                       (sockaddr*)&udp_dest, sizeof(udp_dest));
            }
            total += n;

            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - start).count();
            if (elapsed > 10.0 && total > 0) {
                MLOG_INFO("adb", "Bridge: %.1fs %.2f Mbps",
                         elapsed, total * 8.0 / elapsed / 1e6);
            }
        }

        bridge_connected_ = false;
        closesocket(tcp_sock);
        closesocket(udp_sock);
        MLOG_INFO("adb", "Bridge thread ended (total %lld bytes)", total);
    }
};

} // namespace mirage
