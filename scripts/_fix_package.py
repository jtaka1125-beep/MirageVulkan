path = r"C:\MirageWork\MirageVulkan\src\auto_setup.hpp"
with open(path, "r", encoding="utf-8") as f:
    content = f.read()

old = 'com.example.mirrorsender/.ScreenCaptureService'
new = 'com.mirage.capture/.capture.ScreenCaptureService'

if old in content:
    content = content.replace(old, new)
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
    print("PATCHED")
else:
    print("NOT FOUND")
