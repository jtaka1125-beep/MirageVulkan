path = r"C:\MirageWork\MirageVulkan\src\adb_device_manager.hpp"
with open(path, "r", encoding="utf-8") as f:
    content = f.read()

old = '    int getAssignedPort(const std::string& hardware_id) const;'
new = '    int getAssignedPort(const std::string& hardware_id) const;\n    void setDevicePort(const std::string& hardware_id, int port);'

content = content.replace(old, new, 1)
with open(path, "w", encoding="utf-8") as f:
    f.write(content)
print("HPP done")
