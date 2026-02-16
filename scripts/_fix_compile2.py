path = r"C:\MirageWork\MirageVulkan\src\gui\gui_init.cpp"
with open(path, "r", encoding="utf-8") as f:
    content = f.read()

old = 'success, devices.size());'
new = 'success, device_ids.size());'
content = content.replace(old, new)

with open(path, "w", encoding="utf-8") as f:
    f.write(content)
print("FIXED")
