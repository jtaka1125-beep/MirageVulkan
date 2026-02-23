// =============================================================================
// MirageSystem - TCP Video Receiver (ADB Forward Mode)
// =============================================================================

#include "tcp_video_receiver.hpp"
#include "mirage_log.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#endif

#include <cstring>
#include <cstdio>
#include <array>
#include <chrono>
#include <memory>
#include <algorithm>
#include <functional>
#include <cstdlib>

#include "vid0_parser.hpp"  // Common VID0 parser

static constexpr size_t TCP_RECV_BUF_SIZE = 64 * 1024;

// Reconnect: exponential backoff
static constexpr int RECONNECT_INIT_MS = 2000;
static constexpr int RECONNECT_MAX_MS = 30000;

namespace gui {

namespace {
struct PipeDeleter {
    void operator()(FILE* fp) const { if (fp) pclose(fp); }
};
using UniquePipe = std::unique_ptr<FILE, PipeDeleter>;

// Execute command without showing console window, returns output
std::string execCommandHidden(const std::string& cmd) {
    std::string result;

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hReadPipe = nullptr, hWritePipe = nullptr;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        return result;
    }

    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = nullptr;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    std::string cmd_copy = cmd;

    if (CreateProcessA(nullptr, cmd_copy.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hWritePipe);
        hWritePipe = nullptr;

        char buffer[4096];
        DWORD bytesRead;
        while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0) {
            buffer[bytesRead] = '\0';
            result += buffer;
        }

        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    if (hWritePipe) CloseHandle(hWritePipe);
    CloseHandle(hReadPipe);
#else
    UniquePipe pipe(popen(cmd.c_str(), "r"));
    if (pipe) {
        char buffer[4096];
        while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
            result += buffer;
        }
    }
#endif

    return result;
}
} // anonymous namespace

// ScreenCaptureServiceが動いているかチェック
bool TcpVideoReceiver::isCaptureServiceRunning(const std::string& adb_serial) {
    std::string cmd = "adb -s " + adb_serial + " shell \"dumpsys activity services com.mirage.capture 2>/dev/null\"";
    std::string result = execCommandHidden(cmd);
    return result.find("ScreenCaptureService") != std::string::npos;
}

// MirageCapture TCPミラーモードをintentで起動
void TcpVideoReceiver::launchCaptureTcpMirror(const std::string& adb_serial) {
    // Determine device native size and pass max_size to MirageCapture to avoid implicit downscale.
    // Many capture stacks default to max_size=1440, which turns 1200x2000 -> 864x1440.
    int max_side = 0;
    {
        std::string cmd = "adb -s " + adb_serial + " shell wm size";
        std::string out = execCommandHidden(cmd);
        // Parse "Physical size: WxH" or "Override size: WxH"
        auto parseWH = [&](const std::string& s) {
            int w=0,h=0;
            if (sscanf(s.c_str(), "%*[^0-9]%dx%d", &w, &h) == 2) {
                max_side = std::max(w, h);
            }
        };
        // Prefer Physical size if present
        size_t pos = out.find("Physical size");
        if (pos != std::string::npos) {
            parseWH(out.substr(pos));
        } else {
            parseWH(out);
        }
        if (max_side <= 0) max_side = 2000; // safe default for Npad X1
    }

    // Launch MirageCapture in TCP mirror mode.
    // We pass --ei max_size to request native-ish resolution (long side).
    std::string cmd = "adb -s " + adb_serial +
        " shell am start -n com.mirage.capture/.ui.CaptureActivity"
        " --ez auto_mirror true"
        " --es mirror_mode tcp"
        " --ei max_size " + std::to_string(max_side);
    execCommandHidden(cmd);
    MLOG_INFO("tcpvideo", "Launched MirageCapture TCP mirror (serial=%s, max_size=%d)", adb_serial.c_str(), max_side);
}

TcpVideoReceiver::TcpVideoReceiver() = default;
TcpVideoReceiver::~TcpVideoReceiver() { stop(); }

void TcpVideoReceiver::setDeviceManager(AdbDeviceManager* mgr) { adb_mgr_ = mgr; }

bool TcpVideoReceiver::start(int base_port) {
    if (running_.load()) return true;
    if (!adb_mgr_) { MLOG_ERROR("tcpvideo", "No ADB device manager set"); return false; }

    auto devices = adb_mgr_->getUniqueDevices();
    if (devices.empty()) { MLOG_INFO("tcpvideo", "No devices found"); return false; }

    running_.store(true);
    std::lock_guard<std::mutex> lock(devices_mutex_);
    int port_offset = 0;

    for (const auto& dev : devices) {
        // Use preferred_adb_id (USB preferred, WiFi fallback) instead of requiring USB
        std::string serial = dev.preferred_adb_id;
        if (serial.empty()) {
            // Fallback: try USB first, then WiFi
            if (!dev.usb_connections.empty()) {
                serial = dev.usb_connections[0];
            } else if (!dev.wifi_connections.empty()) {
                serial = dev.wifi_connections[0];
            } else {
                MLOG_INFO("tcpvideo", "Skipping %s (no ADB connection)", dev.display_name.c_str());
                continue;
            }
        }
        int local_port = base_port + port_offset;

        DeviceEntry entry;
        entry.hardware_id = dev.hardware_id;
        entry.adb_serial = serial;
        entry.local_port = local_port;
        entry.pkt_count = 0;
        entry.decoder = std::make_unique<MirrorReceiver>();

        if (!entry.decoder->start_decoder_only()) {
            MLOG_ERROR("tcpvideo", "Failed to start decoder for %s", dev.display_name.c_str());
            continue;
        }

        std::string hw_id = dev.hardware_id;
        entry.thread = std::thread(&TcpVideoReceiver::receiverThread, this, hw_id, serial, local_port);
        MLOG_INFO("tcpvideo", "Started TCP receiver for %s (serial=%s, port=%d)",
                  dev.display_name.c_str(), serial.c_str(), local_port);
        devices_[hw_id] = std::move(entry);
        port_offset++;
    }

    if (devices_.empty()) { running_.store(false); MLOG_WARN("tcpvideo", "No devices available for TCP video"); return false; }
    MLOG_INFO("tcpvideo", "Started %zu TCP video receivers", devices_.size());
    return true;
}

void TcpVideoReceiver::stop() {
    if (!running_.load()) return;
    running_.store(false);
    std::lock_guard<std::mutex> lock(devices_mutex_);
    for (auto& [hw_id, entry] : devices_) {
        if (entry.thread.joinable()) entry.thread.join();
        removeAdbForward(entry.adb_serial, entry.local_port);
    }
    devices_.clear();
    MLOG_INFO("tcpvideo", "Stopped all TCP video receivers");
}

std::vector<std::string> TcpVideoReceiver::getDeviceIds() const {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    std::vector<std::string> ids;
    ids.reserve(devices_.size());
    for (const auto& [hw_id, entry] : devices_) ids.push_back(hw_id);
    return ids;
}

bool TcpVideoReceiver::get_latest_frame(const std::string& hardware_id, MirrorFrame& out) {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    auto it = devices_.find(hardware_id);
    if (it == devices_.end() || !it->second.decoder) return false;
    return it->second.decoder->get_latest_frame(out);
}

bool TcpVideoReceiver::setupAdbForward(const std::string& serial, int local_port) {
    std::string cmd = "adb -s " + serial + " forward tcp:"
                    + std::to_string(local_port) + " tcp:50100 2>&1";
    std::string result = execCommandHidden(cmd);
    if (!result.empty() && result.find("error") != std::string::npos) {
        MLOG_ERROR("tcpvideo", "adb forward failed: %s", result.c_str());
        return false;
    }
    MLOG_INFO("tcpvideo", "ADB forward: tcp:%d -> tcp:50100 (serial=%s)", local_port, serial.c_str());
    return true;
}

void TcpVideoReceiver::removeAdbForward(const std::string& serial, int local_port) {
    std::string cmd = "adb -s " + serial + " forward --remove tcp:" + std::to_string(local_port) + " 2>&1";
    execCommandHidden(cmd);
    MLOG_INFO("tcpvideo", "Removed ADB forward tcp:%d (serial=%s)", local_port, serial.c_str());
}

void TcpVideoReceiver::receiverThread(const std::string& hardware_id,
                                       const std::string& serial,
                                       int local_port) {
    MLOG_INFO("tcpvideo", "Receiver thread started: %s (port %d)", hardware_id.c_str(), local_port);

    int reconnect_delay_ms = RECONNECT_INIT_MS;
    int no_data_count = 0;
    bool forward_established = false;  // forwardが外れた可能性があるので次回再設定
    auto jittered_delay = [](int delay_ms) -> int {
        int jitter = delay_ms * (rand() % 40 - 20) / 100;
        return delay_ms + jitter;
    };

    while (running_.load()) {
        // forward_established: 成功済みなら毎回 adb forward コマンドを叩かない
        if (!forward_established) {
            if (!setupAdbForward(serial, local_port)) {
                MLOG_WARN("tcpvideo", "ADB forward failed for %s, retry in %dms", hardware_id.c_str(), reconnect_delay_ms);
                std::this_thread::sleep_for(std::chrono::milliseconds(jittered_delay(reconnect_delay_ms)));
                reconnect_delay_ms = std::min(reconnect_delay_ms * 2, RECONNECT_MAX_MS);
                continue;
            }
            forward_established = true;
        }

        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) {
            MLOG_ERROR("tcpvideo", "socket() failed for %s", hardware_id.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(jittered_delay(reconnect_delay_ms)));
            reconnect_delay_ms = std::min(reconnect_delay_ms * 2, RECONNECT_MAX_MS);
            continue;
        }

        // SO_LINGER(linger=0): RST即断でTIME_WAIT回避
        struct linger lin = {1, 0};
        setsockopt(sock, SOL_SOCKET, SO_LINGER, (const char*)&lin, sizeof(lin));

#ifdef _WIN32
        DWORD tv = 5000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
        struct timeval tv; tv.tv_sec = 5; tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(local_port));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
#ifdef _WIN32
            int err = WSAGetLastError();
#else
            int err = errno;
#endif
            MLOG_WARN("tcpvideo", "connect() failed for %s (port %d), retry in %dms",
                      hardware_id.c_str(), local_port, reconnect_delay_ms);
            MLOG_WARN("tcpvideo", "connect() errno=%d for %s", err, hardware_id.c_str());
            closesocket(sock);
                        forward_established = false;  // forwardが外れた可能性があるので次回再設定

            // ScreenCaptureServiceが停止していれば自動起動
            if (!isCaptureServiceRunning(serial)) {
                MLOG_INFO("tcpvideo", "ScreenCaptureService not running on %s, sending auto_mirror intent", serial.c_str());
                launchCaptureTcpMirror(serial);
                std::this_thread::sleep_for(std::chrono::milliseconds(3000));
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(jittered_delay(reconnect_delay_ms)));
            reconnect_delay_ms = std::min(reconnect_delay_ms * 2, RECONNECT_MAX_MS);
            continue;
        }

        MLOG_INFO("tcpvideo", "Connected to %s via TCP port %d", hardware_id.c_str(), local_port);

        bool got_data = false;

        std::vector<uint8_t> stream_buffer;
        std::vector<uint8_t> recv_buf(TCP_RECV_BUF_SIZE);

        while (running_.load()) {
            int received = recv(sock, reinterpret_cast<char*>(recv_buf.data()),
                               static_cast<int>(recv_buf.size()), 0);
            if (received > 0) {
                if (!got_data) {
                    got_data = true;
                    reconnect_delay_ms = RECONNECT_INIT_MS;
                }
                stream_buffer.insert(stream_buffer.end(), recv_buf.begin(), recv_buf.begin() + received);
                parseVid0Stream(hardware_id, stream_buffer);
            } else if (received == 0) {
                MLOG_WARN("tcpvideo", "recv() returned 0 (peer closed) for %s", hardware_id.c_str());
                break;
            } else {
#ifdef _WIN32
                int err = WSAGetLastError();
                if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) continue;
#else
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
#endif
                MLOG_WARN("tcpvideo", "recv() error for %s (errno=%d)", hardware_id.c_str(), err);
                break;
            }
        }

        closesocket(sock);

        if (running_.load()) {
            if (!got_data) {
                no_data_count++;
                reconnect_delay_ms = std::min(reconnect_delay_ms * 2, RECONNECT_MAX_MS);
                MLOG_INFO("tcpvideo", "No data from %s (attempt %d), backoff %dms",
                          hardware_id.c_str(), no_data_count, reconnect_delay_ms);

                // ScreenCaptureServiceが停止していれば自動起動を試みる
                if (no_data_count >= 1 && !isCaptureServiceRunning(serial)) {
                    MLOG_INFO("tcpvideo", "ScreenCaptureService not running on %s, sending auto_mirror intent", serial.c_str());
                    launchCaptureTcpMirror(serial);
                    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
                }

            } else {
                no_data_count = 0;  // Reset on successful data
                MLOG_INFO("tcpvideo", "Reconnecting %s in %dms...", hardware_id.c_str(), reconnect_delay_ms);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(jittered_delay(reconnect_delay_ms)));
        }
    }

    MLOG_INFO("tcpvideo", "Receiver thread ended: %s", hardware_id.c_str());
}

void TcpVideoReceiver::parseVid0Stream(const std::string& hardware_id,
                                        std::vector<uint8_t>& buffer) {
    MirrorReceiver* decoder = nullptr;
    uint64_t* pkt_counter = nullptr;
    {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        auto it = devices_.find(hardware_id);
        if (it == devices_.end() || !it->second.decoder) return;
        decoder = it->second.decoder.get();
        pkt_counter = &it->second.pkt_count;
    }

    // Use common VID0 parser from vid0_parser.hpp
    auto result = mirage::video::parseVid0Packets(buffer);

    for (const auto& pkt : result.rtp_packets) {
        (*pkt_counter)++;
        if (*pkt_counter <= 5 || *pkt_counter % 500 == 0) {
            MLOG_INFO("tcpvideo", "VID0 pkt #%llu len=%zu for %s",
                      (unsigned long long)*pkt_counter, pkt.size(), hardware_id.c_str());
        }
        decoder->feed_rtp_packet(pkt.data(), pkt.size());
    }

    if (result.sync_errors > 0) {
        MLOG_WARN("tcpvideo", "VID0 sync errors: %d for %s", result.sync_errors, hardware_id.c_str());
    }
}


// =============================================================================

// =============================================================================

} // namespace gui
