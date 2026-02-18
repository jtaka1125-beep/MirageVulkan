#!/usr/bin/env python3
"""Fix TcpVideoReceiver to use WiFi ADB connections (not just USB)"""

filepath = r'C:\MirageWork\MirageVulkan\src\tcp_video_receiver.cpp'

with open(filepath, 'r', encoding='utf-8-sig') as f:
    content = f.read()

# === FIX: Use preferred_adb_id instead of requiring usb_connections ===
old_start = '''    for (const auto& dev : devices) {
        if (dev.usb_connections.empty()) {
            MLOG_INFO("tcpvideo", "Skipping %s (no USB connection)", dev.display_name.c_str());
            continue;
        }
        const std::string& serial = dev.usb_connections[0];
        int local_port = base_port + port_offset;'''

new_start = '''    for (const auto& dev : devices) {
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
        int local_port = base_port + port_offset;'''

assert old_start in content, "FIX: old start block not found!"
content = content.replace(old_start, new_start)
print("FIX applied: TCP receiver now uses preferred_adb_id (WiFi+USB)")

# Also fix the "No USB devices" warning message
old_msg = '''    if (devices_.empty()) { running_.store(false); MLOG_WARN("tcpvideo", "No USB devices"); return false; }'''
new_msg = '''    if (devices_.empty()) { running_.store(false); MLOG_WARN("tcpvideo", "No devices available for TCP video"); return false; }'''
content = content.replace(old_msg, new_msg)
print("FIX applied: Updated warning message")

with open(filepath, 'w', encoding='utf-8-sig') as f:
    f.write(content)

print("\nAll fixes written to", filepath)
