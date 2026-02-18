#!/usr/bin/env python3
"""Fix: trigger scrcpy launch on connect failures, not just no-data"""

filepath = r'C:\MirageWork\MirageVulkan\src\tcp_video_receiver.cpp'

with open(filepath, 'r', encoding='utf-8-sig') as f:
    content = f.read()

# The connect failure path currently only increases backoff.
# We need to also count connect failures and trigger scrcpy after N failures.

old_connect_fail = '''        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            MLOG_WARN("tcpvideo", "connect() failed for %s (port %d), retry in %dms",
                      hardware_id.c_str(), local_port, reconnect_delay_ms);
            closesocket(sock);
            std::this_thread::sleep_for(std::chrono::milliseconds(reconnect_delay_ms));
            reconnect_delay_ms = std::min(reconnect_delay_ms * 2, RECONNECT_MAX_MS);
            continue;
        }'''

new_connect_fail = '''        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            no_data_count++;  // Count connect failures same as no-data
            MLOG_WARN("tcpvideo", "connect() failed for %s (port %d), attempt %d, retry in %dms",
                      hardware_id.c_str(), local_port, no_data_count, reconnect_delay_ms);
            closesocket(sock);

            // After 2 failed connect attempts, launch scrcpy-server (it creates the listener)
            if (no_data_count == 2 && !scrcpy_launched) {
                MLOG_INFO("tcpvideo", "[%s] TCP connect keeps failing, launching scrcpy-server...", hardware_id.c_str());
                if (launchScrcpyServer(serial, local_port)) {
                    scrcpy_launched = true;
                    reconnect_delay_ms = RECONNECT_INIT_MS;  // Reset backoff after scrcpy launch
                }
            } else {
                reconnect_delay_ms = std::min(reconnect_delay_ms * 2, RECONNECT_MAX_MS);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(reconnect_delay_ms));
            continue;
        }'''

assert old_connect_fail in content, "connect failure block not found!"
content = content.replace(old_connect_fail, new_connect_fail)
print("FIX applied: scrcpy auto-launch on connect failure")

with open(filepath, 'w', encoding='utf-8-sig') as f:
    f.write(content)

print("Written to", filepath)
