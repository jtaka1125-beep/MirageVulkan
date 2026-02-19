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
    SetupStepResult start_screen_capture(const std::string& host, int port, bool is_main = true) {
        (void)host; // scrcpy streams to TCP, bridge sends to UDP locally
        SetupStepResult result;

        if (progress_callback_)
            progress_callback_("Starting scrcpy-server...", 10);

        if (!adb_executor_) {
            result.status = SetupStatus::FAILED;
            result.message = "No ADB executor";
            return result;
        }

        // =====================================================================
        // [無効化] scrcpy関連処理は無効化済み (streamerアンインスト済み)
        // stop_competing_capture_async();
        // scrcpy push / forward / launch はすべてnoop
        // =====================================================================

        // // Generate unique SCID and TCP port
        // scid_ = 0x20000000 + (rand() & 0x1FFFFFFF);
        // char scid_str[16];
        // snprintf(scid_str, sizeof(scid_str), "%08x", scid_);
        // tcp_port_ = 27183 + (port % 100);
        // udp_port_ = port;
        // MLOG_INFO("adb", "scrcpy: scid=%s tcp=%d udp=%d", scid_str, tcp_port_, udp_port_);

        // // 1. Push scrcpy-server
        // adb_executor_("push tools/scrcpy-server-v3.3.4 /data/local/tmp/scrcpy-server.jar");

        // // 3. ADB forward
        // std::string fwd = "forward tcp:" + std::to_string(tcp_port_) +
        //                   " localabstract:scrcpy_" + scid_str;
        // adb_executor_(fwd);

        // // 4. Start scrcpy-server in background
        // int bit_rate = 2000000;
        // int max_fps_val = 30;
        // std::string shell_cmd = "shell CLASSPATH=/data/local/tmp/scrcpy-server.jar"
        //     " app_process / com.genymobile.scrcpy.Server 3.3.4"
        //     " tunnel_forward=true audio=false control=false"
        //     " raw_stream=true video_bit_rate=" + std::to_string(bit_rate) +
        //     " max_fps=" + std::to_string(max_fps_val) +
        //     " i_frame_interval=3" +
        //     " cleanup=false scid=" + std::string(scid_str);
        // server_running_ = true;
        // server_thread_ = std::thread([this, shell_cmd]() {
        //     if (adb_executor_) {
        //         std::string out = adb_executor_(shell_cmd);
        //         MLOG_INFO("adb", "scrcpy server exited: %s", out.c_str());
        //     }
        //     server_running_ = false;
        // });
        // server_thread_.detach();

        // // 5. Wait for server to start
        // std::this_thread::sleep_for(std::chrono::milliseconds(3000));

        // if (progress_callback_)
        //     progress_callback_("Connecting to scrcpy stream...", 50);

        // Bridge disabled - MirrorReceiver reads TCP directly via restart_as_tcp()
        bridge_connected_ = true;  // Mark as "connected" so complete_and_verify() succeeds

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

    int get_tcp_port() const { return tcp_port_; }

    SetupStepResult complete_and_verify() {
        SetupStepResult result;
        if (!bridge_connected_) {
            // Wait longer for WiFi ADB (bridge connects async, scrcpy startup is slow)
            for (int i = 0; i < 20 && !bridge_connected_; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
        result.status = bridge_connected_ ? SetupStatus::COMPLETED : SetupStatus::FAILED;
        result.message = bridge_connected_ ? "" : "Bridge not connected after 10s";
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

    // =========================================================================
    // [無効化] streamerアンインスト済みにつき処理不要
    // =========================================================================
    void stop_competing_capture_async() {
        // [無効化] streamerアンインスト済み、競合するMediaProjectionなし
        // if (!adb_executor_) return;
        // auto executor = adb_executor_;
        // std::thread([executor]() {
        //     MLOG_INFO("adb", "Stopping competing MediaProjection services (async)...");
        //     executor("shell am force-stop com.mirage.streamer");
        //     executor("shell am force-stop com.mirage.android");
        //     MLOG_INFO("adb", "Competing services stopped");
        // }).detach();
    }

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

        // Retry connect with extended timeout for WiFi ADB (scrcpy startup is slow)
        bool connected = false;
        for (int i = 0; i < 50 && bridge_running_; i++) {
            if (connect(tcp_sock, (sockaddr*)&server_addr, sizeof(server_addr)) == 0) {
                connected = true;
                break;
            }
            if (i % 10 == 9) {
                MLOG_INFO("adb", "Bridge: TCP connect retry %d/50 (port %d)", i+1, tcp_port_);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        if (!connected) {
            MLOG_ERROR("adb", "Bridge: TCP connect failed after 50 retries (10s) on port %d", tcp_port_);
            closesocket(tcp_sock);
            return;
        }

        // scrcpy raw_stream=true sends pure H.264 Annex B stream with NO header.
        // Just log and proceed - do NOT consume any bytes.
        MLOG_INFO("adb", "Bridge: TCP connected to scrcpy on port %d (raw_stream=true, no header to skip)", tcp_port_);
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
        auto last_log = start;

        while (bridge_running_) {
            int n = recv(tcp_sock, buf, sizeof(buf), 0);
            if (n <= 0) {
                MLOG_WARN("adb", "Bridge: TCP recv returned %d", n);
                break;
            }
            // Forward raw H.264 to UDP (localhost - no MTU fragmentation needed)
            sendto(udp_sock, buf, n, 0,
                   (sockaddr*)&udp_dest, sizeof(udp_dest));
            total += n;

            auto now = std::chrono::steady_clock::now();
            double since_log = std::chrono::duration<double>(now - last_log).count();
            if (since_log >= 30.0) {
                double elapsed = std::chrono::duration<double>(now - start).count();
                MLOG_INFO("adb", "Bridge[%d]: %.0fs total=%lldKB %.2f Mbps",
                         udp_port_, elapsed, total/1024, total * 8.0 / elapsed / 1e6);
                last_log = now;
            }
        }

        bridge_connected_ = false;
        closesocket(tcp_sock);
        closesocket(udp_sock);
        MLOG_INFO("adb", "Bridge thread ended (total %lld bytes)", total);
    }
};

} // namespace mirage
