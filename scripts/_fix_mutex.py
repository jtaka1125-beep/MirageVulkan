path = r"C:\MirageWork\MirageVulkan\src\multi_device_receiver.cpp"
with open(path, "r", encoding="utf-8") as f:
    content = f.read()

content = content.replace(
    "int MultiDeviceReceiver::getPortForDevice(const std::string& hardware_id) const {\n    std::lock_guard<std::mutex> lock(mutex_);",
    "int MultiDeviceReceiver::getPortForDevice(const std::string& hardware_id) const {\n    std::lock_guard<std::mutex> lock(receivers_mutex_);"
)

with open(path, "w", encoding="utf-8") as f:
    f.write(content)
print("FIXED")
