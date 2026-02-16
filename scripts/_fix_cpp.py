path = r"C:\MirageWork\MirageVulkan\src\adb_device_manager.cpp"
with open(path, "r", encoding="utf-8") as f:
    content = f.read()

# 1. Add setDevicePort after getAssignedPort
old1 = "int AdbDeviceManager::getAssignedPort(const std::string& hardware_id) const {\n    std::lock_guard<std::mutex> lock(mutex_);\n\n    auto it = unique_devices_.find(hardware_id);\n    if (it != unique_devices_.end()) {\n        return it->second.assigned_port;\n    }\n    return 0;\n}"
new1 = old1 + """

void AdbDeviceManager::setDevicePort(const std::string& hardware_id, int port) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = unique_devices_.find(hardware_id);
    if (it != unique_devices_.end()) {
        it->second.assigned_port = port;
        MLOG_INFO("adb", "Assigned port %d to %s", port, it->second.display_name.c_str());
    }
}"""

if old1 in content:
    content = content.replace(old1, new1, 1)
    print("setDevicePort added")
else:
    print("getAssignedPort pattern NOT FOUND")

# 2. Skip assignPorts when base_port is 0
old2 = "    // First assign ports to all devices\n    assignPorts(base_port);"
new2 = "    // Assign ports only if base_port > 0 (otherwise pre-assigned via setDevicePort)\n    if (base_port > 0) {\n        assignPorts(base_port);\n    }"

if old2 in content:
    content = content.replace(old2, new2, 1)
    print("assignPorts skip added")
else:
    print("assignPorts pattern NOT FOUND")

with open(path, "w", encoding="utf-8") as f:
    f.write(content)
print("DONE")
