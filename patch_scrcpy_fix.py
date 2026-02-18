#!/usr/bin/env python3
"""Patch auto_setup.hpp to fix multi-device scrcpy startup issues"""

filepath = r'C:\MirageWork\MirageVulkan\src\auto_setup.hpp'

with open(filepath, 'r', encoding='utf-8-sig') as f:
    content = f.read()

# === FIX 1: Remove global pkill that kills other devices' scrcpy ===
# Also increase wait time and bridge retry timeout

old_start = '''        // 1. Push scrcpy-server (idempotent, fast if already there)
        adb_executor_("push tools/scrcpy-server-v3.3.4 /data/local/tmp/scrcpy-server.jar");

        // 2. Kill any existing scrcpy on this device
        adb_executor_("shell pkill -f scrcpy");
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
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));'''

new_start = '''        // 1. Push scrcpy-server (idempotent, fast if already there)
        adb_executor_("push tools/scrcpy-server-v3.3.4 /data/local/tmp/scrcpy-server.jar");

        // 2. Kill ONLY our own scrcpy (by scid), NOT all scrcpy processes!
        // Using pkill -f scrcpy would kill other devices' scrcpy too.
        // On first launch there's nothing to kill, and on restart the old
        // process with the same scid doesn't exist. So we skip the global kill.
        // If needed, use: shell "pkill -f scid=<our_scid>" (but scid is new each time)

        // 3. ADB forward
        std::string fwd = "forward tcp:" + std::to_string(tcp_port_) +
                          " localabstract:scrcpy_" + scid_str;
        adb_executor_(fwd);

        // 4. Start scrcpy-server in background
        std::string shell_cmd = "shell CLASSPATH=/data/local/tmp/scrcpy-server.jar"
            " app_process / com.genymobile.scrcpy.Server 3.3.4"
            " tunnel_forward=true audio=false control=false"
            " raw_stream=true max_size=800 video_bit_rate=2000000"
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

        // 5. Wait for server to start (WiFi ADB is slower than USB)
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));'''

assert old_start in content, "FIX1: old start_screen_capture block not found!"
content = content.replace(old_start, new_start)
print("FIX 1 applied: removed global pkill, increased startup wait")

# === FIX 2: Increase bridge retry count and add better logging ===
old_bridge_retry = '''        // Retry connect
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
        bridge_connected_ = true;'''

new_bridge_retry = '''        // Retry connect with extended timeout for WiFi ADB (scrcpy startup is slow)
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

        // Read and skip scrcpy device metadata header (device name + codec info)
        // scrcpy sends a header before the raw H.264 stream
        {
            char dummy_name[64] = {};
            int hdr_n = recv(tcp_sock, dummy_name, 64, MSG_WAITALL);
            if (hdr_n == 64) {
                // Read 12-byte codec/size header
                char codec_hdr[12] = {};
                int codec_n = recv(tcp_sock, codec_hdr, 12, MSG_WAITALL);
                if (codec_n == 12) {
                    // Extract width/height from bytes 8-11 (big-endian uint16 each)
                    int w = (static_cast<uint8_t>(codec_hdr[8]) << 8) | static_cast<uint8_t>(codec_hdr[9]);
                    int h = (static_cast<uint8_t>(codec_hdr[10]) << 8) | static_cast<uint8_t>(codec_hdr[11]);
                    MLOG_INFO("adb", "Bridge: scrcpy header: device='%.64s' codec=%02x%02x%02x%02x size=%dx%d",
                             dummy_name,
                             (uint8_t)codec_hdr[0], (uint8_t)codec_hdr[1],
                             (uint8_t)codec_hdr[2], (uint8_t)codec_hdr[3],
                             w, h);
                } else {
                    MLOG_WARN("adb", "Bridge: short codec header read: %d/12", codec_n);
                }
            } else {
                MLOG_WARN("adb", "Bridge: short device name read: %d/64", hdr_n);
            }
        }

        MLOG_INFO("adb", "Bridge: TCP connected to scrcpy on port %d (header consumed)", tcp_port_);
        bridge_connected_ = true;'''

assert old_bridge_retry in content, "FIX2: old bridge retry block not found!"
content = content.replace(old_bridge_retry, new_bridge_retry)
print("FIX 2 applied: extended retry, added scrcpy header consumption")

# === FIX 3: complete_and_verify - longer wait for WiFi ===
old_verify = '''    SetupStepResult complete_and_verify() {
        SetupStepResult result;
        if (bridge_connected_) {
            result.status = SetupStatus::COMPLETED;
            result.message = "";
        } else {
            // Give it a moment
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            result.status = bridge_connected_ ? SetupStatus::COMPLETED : SetupStatus::FAILED;
            result.message = bridge_connected_ ? "" : "Bridge not connected";
        }'''

new_verify = '''    SetupStepResult complete_and_verify() {
        SetupStepResult result;
        if (!bridge_connected_) {
            // Wait longer for WiFi ADB (bridge connects async, scrcpy startup is slow)
            for (int i = 0; i < 20 && !bridge_connected_; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
        result.status = bridge_connected_ ? SetupStatus::COMPLETED : SetupStatus::FAILED;
        result.message = bridge_connected_ ? "" : "Bridge not connected after 10s";'''

assert old_verify in content, "FIX3: old complete_and_verify block not found!"
content = content.replace(old_verify, new_verify)
print("FIX 3 applied: extended verify wait for WiFi ADB")

with open(filepath, 'w', encoding='utf-8-sig') as f:
    f.write(content)

print("\nAll fixes written to", filepath)
