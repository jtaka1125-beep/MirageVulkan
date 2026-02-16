import re

path = r"C:\MirageWork\MirageVulkan\src\multi_device_receiver.cpp"
with open(path, "r", encoding="utf-8") as f:
    content = f.read()

old = """        // Use port=0 for OS-assigned port (or base_port if specified)
        uint16_t request_port = (base_port == 0) ? 0 : base_port++;"""

new = """        // Always use port=0 for OS-assigned port to avoid TIME_WAIT conflicts
        uint16_t request_port = 0;
        (void)base_port;  // base_port kept for API compatibility"""

content = content.replace(old, new)

with open(path, "w", encoding="utf-8") as f:
    f.write(content)

print("OK" if "Always use port=0" in content else "FAILED")
