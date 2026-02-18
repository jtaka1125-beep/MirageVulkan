#!/usr/bin/env python3
"""Fix tcp_video_receiver.cpp: connect failures trigger scrcpy auto-launch"""
import sys

filepath = r'C:\MirageWork\MirageVulkan\src\tcp_video_receiver.cpp'

with open(filepath, 'r', encoding='utf-8-sig') as f:
    content = f.read()

# Key change: in the connect() failure block, increment fail counter + trigger scrcpy
# Original has: connect fail -> backoff -> continue (no_data_count never incremented)
# Fix: connect fail -> fail_count++ -> if fail_count>=2 launch scrcpy

old_block = '''    int reconnect_delay_ms = RECONNECT_INIT_MS;
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
        }'''

new_block = '''    int reconnect_delay_ms = RECONNECT_INIT_MS;
    int fail_count = 0;       // Counts ALL failures (connect + no-data)
    bool scrcpy_launched = false;

    while (running_.load()) {
        // After 2 failures of any kind, launch scrcpy-server as fallback
        if (fail_count >= 2 && !scrcpy_launched) {
            MLOG_INFO("tcpvideo", "[%s] %d failures, launching scrcpy-server...", hardware_id.c_str(), fail_count);
            if (launchScrcpyServer(serial, local_port)) {
                scrcpy_launched = true;
                reconnect_delay_ms = RECONNECT_INIT_MS;
            }
        }

        // Only set up APK-mode forward if scrcpy hasn't taken over
        if (!scrcpy_launched) {
            if (!setupAdbForward(serial, local_port)) {
                fail_count++;
                MLOG_WARN("tcpvideo", "ADB forward failed for %s (fail #%d)", hardware_id.c_str(), fail_count);
                std::this_thread::sleep_for(std::chrono::milliseconds(reconnect_delay_ms));
                reconnect_delay_ms = std::min(reconnect_delay_ms * 2, RECONNECT_MAX_MS);
                continue;
            }
        }

        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) {
            fail_count++;
            MLOG_ERROR("tcpvideo", "socket() failed for %s (fail #%d)", hardware_id.c_str(), fail_count);
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
            fail_count++;
            MLOG_WARN("tcpvideo", "connect() failed for %s (port %d, fail #%d)",
                      hardware_id.c_str(), local_port, fail_count);
            closesocket(sock);
            std::this_thread::sleep_for(std::chrono::milliseconds(reconnect_delay_ms));
            reconnect_delay_ms = std::min(reconnect_delay_ms * 2, RECONNECT_MAX_MS);
            continue;
        }'''

if old_block not in content:
    print("ERROR: old block not found!")
    sys.exit(1)

content = content.replace(old_block, new_block)
print("FIX 1: connect failures now count toward scrcpy trigger")

# Also fix the no_data section at the bottom of the loop
old_nodata = '''            if (!got_data) {
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
            }'''

new_nodata = '''            if (!got_data) {
                fail_count++;
                reconnect_delay_ms = std::min(reconnect_delay_ms * 2, RECONNECT_MAX_MS);
                MLOG_INFO("tcpvideo", "No data from %s (fail #%d), backoff %dms",
                          hardware_id.c_str(), fail_count, reconnect_delay_ms);
            } else {
                fail_count = 0;  // Reset on successful data
                reconnect_delay_ms = RECONNECT_INIT_MS;
                MLOG_INFO("tcpvideo", "Reconnecting %s in %dms...", hardware_id.c_str(), reconnect_delay_ms);
            }'''

if old_nodata not in content:
    print("ERROR: old no_data block not found!")
    sys.exit(1)

content = content.replace(old_nodata, new_nodata)
print("FIX 2: unified fail_count replaces no_data_count")

with open(filepath, 'w', encoding='utf-8-sig') as f:
    f.write(content)

print("All fixes written to", filepath)
