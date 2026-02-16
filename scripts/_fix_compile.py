path = r"C:\MirageWork\MirageVulkan\src\gui\gui_init.cpp"
with open(path, "r", encoding="utf-8") as f:
    content = f.read()

content = content.replace(
    'MLOG_INFO("gui", "Screen capture started: %d/%zu devices", success, devices.size());',
    'MLOG_INFO("gui", "Screen capture started: %d/%zu devices", success, device_ids.size());'
)

with open(path, "w", encoding="utf-8") as f:
    f.write(content)
print("FIXED")
