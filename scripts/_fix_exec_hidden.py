path = r"C:\MirageWork\MirageVulkan\src\adb_device_manager.cpp"
with open(path, "r", encoding="utf-8") as f:
    content = f.read()

# Fix execCommandHidden to use cmd /c internally
old = '    PROCESS_INFORMATION pi = {};\n    std::string cmd_copy = cmd;'
new = '    PROCESS_INFORMATION pi = {};\n    std::string cmd_copy = "cmd /c " + cmd;'

if old in content:
    content = content.replace(old, new, 1)
    
    # Revert the earlier parseAdbDevices fix since execCommandHidden now handles it
    content = content.replace(
        '    std::string cmd = "cmd /c adb devices 2>&1";',
        '    std::string cmd = "adb devices 2>&1";'
    )
    
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
    print("PATCHED: execCommandHidden now uses cmd /c prefix")
else:
    print("NOT FOUND")
