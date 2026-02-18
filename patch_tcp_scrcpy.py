#!/usr/bin/env python3
"""
Patch tcp_video_receiver.cpp to:
1. Auto-detect VID0 vs raw H.264 streams
2. Auto-launch scrcpy-server when APK is not responding
"""

filepath = r'C:\MirageWork\MirageVulkan\src\tcp_video_receiver.cpp'

with open(filepath, 'r', encoding='utf-8-sig') as f:
    content = f.read()

# ============================================================
# PATCH 1: Add scrcpy launcher + raw H.264 parser method
# ============================================================

# Insert new methods and includes before the final } // namespace gui
old_tail = '''} // namespace gui'''

new_methods_and_tail = r'''
// =============================================================================
// scrcpy-server auto-launch (fallback when APK not running)
// =============================================================================

bool TcpVideoReceiver::launchScrcpyServer(const std::string& serial, int local_port) {
    // Generate unique SCID for this session
    uint32_t scid = static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count() & 0x7FFFFFFF) | 0x20000000;
    char scid_str[16];
    snprintf(scid_str, sizeof(scid_str), "%08x", scid);

    MLOG_INFO("tcpvideo", "[scrcpy] Launching for %s (scid=%s)", serial.c_str(), scid_str);

    // Kill any existing scrcpy process on device
    std::string kill_cmd = "adb -s " + serial + " shell pkill -f scrcpy 2>&1";
    execCommandHidden(kill_cmd);

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

    // Start scrcpy-server in background (fire-and-forget via shell)
    std::string start_cmd = "adb -s " + serial + " shell "
        "\"CLASSPATH=/data/local/tmp/scrcpy-server.jar "
        "app_process / com.genymobile.scrcpy.Server 3.3.4 "
        "tunnel_forward=true audio=false control=false "
        "raw_stream=true max_size=800 video_bit_rate=2000000 "
        "max_fps=30 cleanup=false scid=" + std::string(scid_str) + "\" 2>&1";

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
    {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        auto it = devices_.find(hardware_id);
        if (it == devices_.end() || !it->second.decoder) return;
        decoder = it->second.decoder.get();
    }

    // Feed raw H.264 Annex B data directly to decoder's process_raw_h264
    // The MirrorReceiver will handle NAL unit splitting internally
    if (!buffer.empty()) {
        decoder->process_raw_h264(buffer.data(), buffer.size());
        buffer.clear();
    }
}

} // namespace gui'''

assert old_tail in content, "PATCH1: tail not found!"
content = content.replace(old_tail, new_methods_and_tail, 1)  # Replace only last occurrence
print("PATCH 1 applied: Added scrcpy launcher + raw H.264 parser")


# ============================================================
# PATCH 2: Modify receiverThread to auto-detect stream type
#           and launch scrcpy on repeated "No data"
# ============================================================

# Replace the main receive loop section
old_recv_loop = '''        bool got_data = false;
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
        }'''

new_recv_loop = '''        bool got_data = false;
        bool is_raw_h264 = false;  // auto-detected on first data
        bool format_detected = false;
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
        }'''

assert old_recv_loop in content, "PATCH2: recv loop not found!"
content = content.replace(old_recv_loop, new_recv_loop)
print("PATCH 2 applied: Auto-detect VID0 vs raw H.264")


# ============================================================
# PATCH 3: Add scrcpy auto-launch on repeated "No data"
# ============================================================

old_no_data = '''        if (running_.load()) {
            if (!got_data) {
                reconnect_delay_ms = std::min(reconnect_delay_ms * 2, RECONNECT_MAX_MS);
                MLOG_INFO("tcpvideo", "No data from %s, backoff %dms", hardware_id.c_str(), reconnect_delay_ms);
            } else {
                MLOG_INFO("tcpvideo", "Reconnecting %s in %dms...", hardware_id.c_str(), reconnect_delay_ms);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(reconnect_delay_ms));
        }'''

new_no_data = '''        if (running_.load()) {
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
        }'''

assert old_no_data in content, "PATCH3: no_data block not found!"
content = content.replace(old_no_data, new_no_data)
print("PATCH 3 applied: scrcpy auto-launch on repeated No data")


# ============================================================
# PATCH 4: Add counter variables at start of receiverThread
# ============================================================

old_thread_start = '''    int reconnect_delay_ms = RECONNECT_INIT_MS;'''

new_thread_start = '''    int reconnect_delay_ms = RECONNECT_INIT_MS;
    int no_data_count = 0;
    bool scrcpy_launched = false;'''

assert old_thread_start in content, "PATCH4: thread start not found!"
content = content.replace(old_thread_start, new_thread_start, 1)
print("PATCH 4 applied: Added no_data_count and scrcpy_launched vars")


with open(filepath, 'w', encoding='utf-8-sig') as f:
    f.write(content)

print("\nAll patches written to", filepath)
