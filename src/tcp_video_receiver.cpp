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
    bool scrcpy_launched = false;

    while (running_.load()) {
        if (!setupAdbForward(serial, local_port)) {
            MLOG_WARN("tcpvideo", "ADB forward failed for %s, retry in %dms", hardware_id.c_str(), reconnect_delay_ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(reconnect_delay_ms));
            reconnect_delay_ms = std::min(reconnect_delay_ms * 2, RECONNECT_MAX_MS);
            continue;
        }

        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) {
            MLOG_ERROR("tcpvideo", "socket() failed for %s", hardware_id.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(reconnect_delay_ms));
            reconnect_delay_ms = std::min(reconnect_delay_ms * 2, RECONNECT_MAX_MS);
            continue;
        }

#ifdef _WIN32
        DWORD tv = 2000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
        struct timeval tv; tv.tv_sec = 2; tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(local_port));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            MLOG_WARN("tcpvideo", "connect() failed for %s (port %d), retry in %dms",
                      hardware_id.c_str(), local_port, reconnect_delay_ms);
            closesocket(sock);
            std::this_thread::sleep_for(std::chrono::milliseconds(reconnect_delay_ms));
            reconnect_delay_ms = std::min(reconnect_delay_ms * 2, RECONNECT_MAX_MS);
            continue;
        }

        MLOG_INFO("tcpvideo", "Connected to %s via TCP port %d", hardware_id.c_str(), local_port);

        bool got_data = false;
        bool is_raw_h264 = false;  // auto-detected on first data
        bool format_detected = false;

        // scrcpy sends codec header on each new connection
        {
            std::lock_guard<std::mutex> lock(devices_mutex_);
            auto it = devices_.find(hardware_id);
            if (it != devices_.end()) it->second.header_skipped = false;
        }
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

                // Auto-detect stream format on first data chunk
                if (!format_detected && stream_buffer.size() >= 4) {
                    if (stream_buffer[0] == 0x56 && stream_buffer[1] == 0x49 &&
                        stream_buffer[2] == 0x44 && stream_buffer[3] == 0x30) {
                        is_raw_h264 = false;
                        MLOG_INFO("tcpvideo", "[%s] Detected VID0 stream format", hardware_id.c_str());
                    } else {
                        is_raw_h264 = true;
                        MLOG_INFO("tcpvideo", "[%s] Detected raw H.264 stream (scrcpy mode)", hardware_id.c_str());
                    }
                    format_detected = true;
                }

                if (format_detected) {
                    if (is_raw_h264) {
                        parseRawH264Stream(hardware_id, stream_buffer);
                    } else {
                        parseVid0Stream(hardware_id, stream_buffer);
                    }
                }
            } else if (received == 0) {
                break;
            } else {
#ifdef _WIN32
                int err = WSAGetLastError();
                if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) continue;
#else
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
#endif
                MLOG_WARN("tcpvideo", "recv() error for %s", hardware_id.c_str());
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

                // After 2 failed attempts, try launching scrcpy-server
                if (no_data_count == 2 && !scrcpy_launched) {
                    MLOG_INFO("tcpvideo", "[%s] APK not responding, launching scrcpy-server...", hardware_id.c_str());
                    if (launchScrcpyServer(serial, local_port)) {
                        scrcpy_launched = true;
                        reconnect_delay_ms = RECONNECT_INIT_MS;  // Reset backoff after scrcpy launch
                    }
                }
            } else {
                no_data_count = 0;  // Reset on successful data
                MLOG_INFO("tcpvideo", "Reconnecting %s in %dms...", hardware_id.c_str(), reconnect_delay_ms);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(reconnect_delay_ms));
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
// scrcpy-server auto-launch (fallback when APK not running)
// =============================================================================

bool TcpVideoReceiver::launchScrcpyServer(const std::string& serial, int local_port) {
    // Generate unique SCID for this session
    uint32_t scid = static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count() & 0x0FFFFFFF) | 0x10000000;
    char scid_str[16];
    snprintf(scid_str, sizeof(scid_str), "%08x", scid);

    MLOG_INFO("tcpvideo", "[scrcpy] Launching for %s (scid=%s)", serial.c_str(), scid_str);

    // Kill any existing scrcpy process on device
    std::string kill_cmd = "adb -s " + serial + " shell \"pkill -f scrcpy 2>/dev/null; pkill -f app_process 2>/dev/null\"";
    execCommandHidden(kill_cmd);
    // Wait for LocalServerSocket release after killing scrcpy
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    // Remove old forward
    std::string rm_fwd = "adb -s " + serial + " forward --remove tcp:" + std::to_string(local_port) + " 2>&1";
    execCommandHidden(rm_fwd);

    // Push scrcpy-server jar (idempotent)
    std::string push_cmd = "adb -s " + serial + " push tools\\scrcpy-server-v3.3.4 /data/local/tmp/scrcpy-server.jar 2>&1";
    std::string push_result = execCommandHidden(push_cmd);
    MLOG_INFO("tcpvideo", "[scrcpy] push: %s", push_result.c_str());

    // Setup forward to scrcpy abstract socket
    std::string abstract_name = std::string("localabstract:scrcpy_") + scid_str;
    std::string fwd_cmd = "adb -s " + serial + " forward tcp:" + std::to_string(local_port) + " " + abstract_name + " 2>&1";
    std::string fwd_result = execCommandHidden(fwd_cmd);
    if (fwd_result.find("error") != std::string::npos) {
        MLOG_ERROR("tcpvideo", "[scrcpy] forward failed: %s", fwd_result.c_str());
        return false;
    }

    // Start scrcpy-server in background (fire-and-forget via CreateProcess)
    // NOTE: No quotes around shell command — Windows CreateProcess passes args directly
    std::string start_cmd = "adb -s " + serial + " shell "
        "CLASSPATH=/data/local/tmp/scrcpy-server.jar "
        "app_process / com.genymobile.scrcpy.Server 3.3.4 "
        "tunnel_forward=true audio=false control=false "
        "raw_stream=true max_size=800 video_bit_rate=2000000 "
        "max_fps=30 cleanup=false scid=" + std::string(scid_str);

    // Launch async: we use CreateProcess with no-wait
#ifdef _WIN32
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::string cmd_line = start_cmd;
    if (CreateProcessA(nullptr, cmd_line.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        MLOG_INFO("tcpvideo", "[scrcpy] Server process launched for %s", serial.c_str());
    } else {
        MLOG_ERROR("tcpvideo", "[scrcpy] CreateProcess failed for %s", serial.c_str());
        return false;
    }
#else
    std::string bg_cmd = start_cmd + " &";
    system(bg_cmd.c_str());
#endif

    // Wait for server to initialize
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    MLOG_INFO("tcpvideo", "[scrcpy] Ready, forward tcp:%d -> %s", local_port, abstract_name.c_str());
    return true;
}

// =============================================================================
// Raw H.264 stream parser (for scrcpy raw_stream=true)
// =============================================================================

void TcpVideoReceiver::parseRawH264Stream(const std::string& hardware_id,
                                            std::vector<uint8_t>& buffer) {
    MirrorReceiver* decoder = nullptr;
    bool* header_skipped = nullptr;
    {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        auto it = devices_.find(hardware_id);
        if (it == devices_.end() || !it->second.decoder) return;
        decoder = it->second.decoder.get();
        header_skipped = &it->second.header_skipped;
    }

    if (!*header_skipped) {
        // scrcpy raw_stream=true sends 12-byte codec header first:
        // codec_id (4 bytes) + width (4 bytes) + height (4 bytes)
        if (buffer.size() < 12) return;  // ヘッダ未到着、次のrecvを待つ

        uint32_t codec_id = (buffer[0] << 24) | (buffer[1] << 16) | (buffer[2] << 8) | buffer[3];
        uint32_t width = (buffer[4] << 24) | (buffer[5] << 16) | (buffer[6] << 8) | buffer[7];
        uint32_t height = (buffer[8] << 24) | (buffer[9] << 16) | (buffer[10] << 8) | buffer[11];
        MLOG_INFO("tcpvideo", "[%s] scrcpy codec header: id=%u, %ux%u",
                  hardware_id.c_str(), codec_id, width, height);

        buffer.erase(buffer.begin(), buffer.begin() + 12);
        *header_skipped = true;
    }

    // Feed raw H.264 Annex B data directly to decoder's process_raw_h264
    if (!buffer.empty()) {
        decoder->process_raw_h264(buffer.data(), buffer.size());
        buffer.clear();
    }
}

} // namespace gui
