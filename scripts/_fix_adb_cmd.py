path = r"C:\MirageWork\MirageVulkan\src\adb_device_manager.cpp"
with open(path, "r", encoding="utf-8") as f:
    content = f.read()

# Fix 1: adb devices command
old = '    std::string cmd = "adb devices 2>&1";'
new = '    std::string cmd = "cmd /c adb devices 2>&1";'
content = content.replace(old, new, 1)

# Also check execCommandHidden for the same issue in adbCommand
# adbCommand also uses execCommandHidden
count = content.count('execCommandHidden')
print(f"execCommandHidden usages: {count}")

with open(path, "w", encoding="utf-8") as f:
    f.write(content)
print("PATCHED: adb devices -> cmd /c adb devices")
