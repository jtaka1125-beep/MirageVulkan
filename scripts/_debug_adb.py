import re

path = r"C:\MirageWork\MirageVulkan\src\adb_device_manager.cpp"
with open(path, "r", encoding="utf-8") as f:
    content = f.read()

old_line = '    std::string result = execCommandHidden(cmd);\n    if (result.empty()) {'
new_line = '    std::string result = execCommandHidden(cmd);\n    MLOG_INFO("adb", "Raw adb output (%zu bytes): [%s]", result.size(), result.substr(0, 500).c_str());\n    if (result.empty()) {'

if old_line in content:
    content = content.replace(old_line, new_line, 1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
    print("PATCHED")
else:
    print("NOT FOUND")
