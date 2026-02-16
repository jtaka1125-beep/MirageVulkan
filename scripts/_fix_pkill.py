path = r"C:\MirageWork\MirageVulkan\src\auto_setup.hpp"
with open(path, 'r', encoding='utf-8') as f:
    c = f.read()
c = c.replace(
    'adb_executor_("shell pkill -f scrcpy 2>/dev/null");',
    'adb_executor_("shell pkill -f scrcpy");'
)
with open(path, 'w', encoding='utf-8') as f:
    f.write(c)
print("FIXED pkill redirect")
