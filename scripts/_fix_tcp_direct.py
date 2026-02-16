"""
Modify the flow:
1. AutoSetup: remove bridge, expose tcp_port
2. adb_device_manager: after scrcpy start, restart MirrorReceiver in TCP mode
3. multi_device_receiver: add method to restart a receiver in TCP mode
"""

# === 1. AutoSetup: remove bridge, add get_tcp_port() ===
path = r"C:\MirageWork\MirageVulkan\src\auto_setup.hpp"
with open(path, 'r', encoding='utf-8') as f:
    c = f.read()

# Add getter for tcp_port
if 'get_tcp_port' not in c:
    c = c.replace(
        '    SetupStepResult complete_and_verify() {',
        '    int get_tcp_port() const { return tcp_port_; }\n\n    SetupStepResult complete_and_verify() {'
    )

# Remove bridge thread launch from start_screen_capture
# Replace the bridge launch section with just returning
old_bridge_launch = '''        // 5. Wait for server to start, then launch TCP->UDP bridge
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));

        if (progress_callback_)
            progress_callback_("Connecting to scrcpy stream...", 50);

        // Start bridge thread
        bridge_running_ = true;
        bridge_thread_ = std::thread(&AutoSetup::bridge_loop, this);'''

new_bridge_launch = '''        // 5. Wait for server to start
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));

        if (progress_callback_)
            progress_callback_("scrcpy server ready, TCP port: " + std::to_string(tcp_port_), 50);

        // No bridge needed - MirrorReceiver connects directly via TCP'''

c = c.replace(old_bridge_launch, new_bridge_launch)

with open(path, 'w', encoding='utf-8') as f:
    f.write(c)

# === 2. adb_device_manager: restart receiver in TCP mode ===
cpp_path = r"C:\MirageWork\MirageVulkan\src\adb_device_manager.cpp"
with open(cpp_path, 'r', encoding='utf-8') as f:
    d = f.read()

# After start_screen_capture succeeds, restart MirrorReceiver in TCP mode
# Find the return statement at the end of startScreenCapture
old_return = '''    return (result2.status == mirage::SetupStatus::COMPLETED ||
            result2.status == mirage::SetupStatus::SKIPPED) &&
           (result3.status == mirage::SetupStatus::COMPLETED);
}'''

new_return = '''    bool ok = (result2.status == mirage::SetupStatus::COMPLETED ||
              result2.status == mirage::SetupStatus::SKIPPED) &&
             (result3.status == mirage::SetupStatus::COMPLETED);

    if (ok) {
        int tcp_port = setup.get_tcp_port();
        MLOG_INFO("adb", "Success (port %d) - TCP mode on port %d", port, tcp_port);
        // Store TCP port for multi_device_receiver to use
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [key, ud] : unique_devices_) {
                if (ud.preferred_adb_id == adb_id) {
                    ud.assigned_tcp_port = tcp_port;
                    break;
                }
            }
        }
    }
    return ok;
}'''

d = d.replace(old_return, new_return)

with open(cpp_path, 'w', encoding='utf-8') as f:
    f.write(d)

# === 3. Add assigned_tcp_port to UniqueDevice ===
hpp_path = r"C:\MirageWork\MirageVulkan\src\adb_device_manager.hpp"
with open(hpp_path, 'r', encoding='utf-8') as f:
    h = f.read()

if 'assigned_tcp_port' not in h:
    h = h.replace(
        '        int assigned_port = 0;',
        '        int assigned_port = 0;\n        int assigned_tcp_port = 0;  // scrcpy TCP port (from AutoSetup)'
    )

with open(hpp_path, 'w', encoding='utf-8') as f:
    f.write(h)

# === 4. multi_device_receiver: add restart_as_tcp method ===
mdr_hpp = r"C:\MirageWork\MirageVulkan\src\multi_device_receiver.hpp"
with open(mdr_hpp, 'r', encoding='utf-8') as f:
    m = f.read()

if 'restart_as_tcp' not in m:
    m = m.replace(
        '    // Feed RTP packet to the first device\'s receiver (for USB video)',
        '    // Restart a device receiver in TCP mode (scrcpy direct connection)\n'
        '    bool restart_as_tcp(const std::string& hardware_id, uint16_t tcp_port);\n\n'
        '    // Feed RTP packet to the first device\'s receiver (for USB video)'
    )

with open(mdr_hpp, 'w', encoding='utf-8') as f:
    f.write(m)

# Add implementation to cpp
mdr_cpp = r"C:\MirageWork\MirageVulkan\src\multi_device_receiver.cpp"
with open(mdr_cpp, 'r', encoding='utf-8') as f:
    mc = f.read()

if 'restart_as_tcp' not in mc:
    # Insert before stop()
    mc = mc.replace(
        'void MultiDeviceReceiver::stop() {',
        '''bool MultiDeviceReceiver::restart_as_tcp(const std::string& hardware_id, uint16_t tcp_port) {
    std::lock_guard<std::mutex> lock(receivers_mutex_);
    auto it = receivers_.find(hardware_id);
    if (it == receivers_.end()) {
        MLOG_ERROR("multi", "restart_as_tcp: device %s not found", hardware_id.c_str());
        return false;
    }

    auto& entry = it->second;
    int old_port = entry.port;

    // Stop existing UDP receiver
    if (entry.receiver) {
        entry.receiver->stop();
    }

    // Create new receiver in TCP mode
    entry.receiver = std::make_unique<MirrorReceiver>();
    if (entry.receiver->start_tcp(tcp_port)) {
        entry.port = tcp_port;
        port_to_device_.erase(old_port);
        port_to_device_[tcp_port] = hardware_id;
        MLOG_INFO("multi", "Restarted %s in TCP mode on port %d (was UDP %d)",
                  entry.display_name.c_str(), tcp_port, old_port);
        return true;
    } else {
        MLOG_ERROR("multi", "Failed to restart %s in TCP mode on port %d",
                   entry.display_name.c_str(), tcp_port);
        return false;
    }
}

void MultiDeviceReceiver::stop() {'''
    )

with open(mdr_cpp, 'w', encoding='utf-8') as f:
    f.write(mc)

print("DONE: TCP direct mode plumbing complete")
