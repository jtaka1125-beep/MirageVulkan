from pathlib import Path
import re

p = Path(r"C:/MirageWork/MirageVulkan/src/gui/gui_init.cpp")
t = p.read_text(encoding='utf-8', errors='ignore')

# 1) Replace legacy SET_FPS broadcast with new ACTION_VIDEO_FPS
# Try to find the command string containing com.mirage.capture.SET_FPS
if 'com.mirage.capture.SET_FPS' in t:
    t = t.replace('com.mirage.capture.SET_FPS', 'com.mirage.capture.ACTION_VIDEO_FPS')

# 2) If legacy extra name differs, ensure it uses --ei fps <n> (already)

# 3) Force X1 as first(main) device in TCP-only registration block
# We locate the block that iterates devices and uses 'bool first = true;'
# Then we sort so that display_name containing 'Npad X1' comes first.
if 'Force X1 as main' not in t:
    # Insert a small reorder right after devices are obtained in initializeRouting()
    m = re.search(r"auto\s+devices\s*=\s+g_adb_manager->getUniqueDevices\(\);", t)
    if m:
        insert = "\n        // Force X1 as main in TCP-only mode (prevents adaptive downscale to 1072)\n" \
                 "        std::stable_sort(devices.begin(), devices.end(), [](const auto& a, const auto& b){\n" \
                 "            const bool ax1 = a.display_name.find(\"Npad X1\") != std::string::npos;\n" \
                 "            const bool bx1 = b.display_name.find(\"Npad X1\") != std::string::npos;\n" \
                 "            return (ax1 && !bx1);\n" \
                 "        });\n"
        t = t[:m.end()] + insert + t[m.end():]

# 4) Also update legacy bitrate action if present
if 'com.mirage.capture.SET_BITRATE' in t:
    t = t.replace('com.mirage.capture.SET_BITRATE', 'com.mirage.capture.ACTION_VIDEO_BITRATE')

p.write_text(t, encoding='utf-8')
print('patched gui_init.cpp: X1 main + ACTION_VIDEO_FPS')
