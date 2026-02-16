import re

# 1. Add getPortForDevice to multi_device_receiver.hpp
path_hpp = r"C:\MirageWork\MirageVulkan\src\multi_device_receiver.hpp"
with open(path_hpp, "r", encoding="utf-8") as f:
    content = f.read()

old_hpp = '    std::vector<std::string> getDeviceIds() const;'
new_hpp = '''    std::vector<std::string> getDeviceIds() const;
    int getPortForDevice(const std::string& hardware_id) const;'''

content = content.replace(old_hpp, new_hpp, 1)
with open(path_hpp, "w", encoding="utf-8") as f:
    f.write(content)
print("HPP patched")

# 2. Add implementation to multi_device_receiver.cpp
path_cpp = r"C:\MirageWork\MirageVulkan\src\multi_device_receiver.cpp"
with open(path_cpp, "r", encoding="utf-8") as f:
    content = f.read()

# Find end of getDeviceIds implementation and add after it
old_cpp = 'std::vector<std::string> MultiDeviceReceiver::getDeviceIds() const {'
# Find the method and insert after the closing brace
idx = content.find(old_cpp)
if idx >= 0:
    # Find closing brace of getDeviceIds
    brace_count = 0
    i = content.index('{', idx)
    for j in range(i, len(content)):
        if content[j] == '{': brace_count += 1
        if content[j] == '}': brace_count -= 1
        if brace_count == 0:
            insert_pos = j + 1
            break
    
    new_method = '''

int MultiDeviceReceiver::getPortForDevice(const std::string& hardware_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = receivers_.find(hardware_id);
    if (it != receivers_.end()) return it->second.port;
    return 0;
}'''
    content = content[:insert_pos] + new_method + content[insert_pos:]
    with open(path_cpp, "w", encoding="utf-8") as f:
        f.write(content)
    print("CPP patched")
else:
    print("CPP: getDeviceIds not found")

# 3. Modify gui_init.cpp to use receiver ports instead of assignPorts
path_gui = r"C:\MirageWork\MirageVulkan\src\gui\gui_init.cpp"
with open(path_gui, "r", encoding="utf-8") as f:
    content = f.read()

# Replace the startScreenCaptureOnAll call with per-device port assignment
old_gui = '''        // Port info is logged by assignPorts() in AdbDeviceManager

        // Auto-start screen capture
        std::string host_ip = mirage::config::getConfig().network.pc_ip;
        int success = g_adb_manager->startScreenCaptureOnAll(host_ip, mirage::config::getConfig().network.video_base_port);
        MLOG_INFO("gui", "Screen capture started: %d/%zu devices", success, device_ids.size());'''

new_gui = '''        // Assign actual receiver ports to devices (not config base_port)
        for (const auto& hw_id : device_ids) {
            int actual_port = g_multi_receiver->getPortForDevice(hw_id);
            if (actual_port > 0) {
                g_adb_manager->setDevicePort(hw_id, actual_port);
            }
        }

        // Auto-start screen capture using actual receiver ports
        std::string host_ip = mirage::config::getConfig().network.pc_ip;
        int success = g_adb_manager->startScreenCaptureOnAll(host_ip, 0);  // 0 = use pre-assigned ports
        MLOG_INFO("gui", "Screen capture started: %d/%zu devices", success, device_ids.size());'''

if old_gui in content:
    content = content.replace(old_gui, new_gui, 1)
    with open(path_gui, "w", encoding="utf-8") as f:
        f.write(content)
    print("GUI patched")
else:
    print("GUI: pattern not found")
