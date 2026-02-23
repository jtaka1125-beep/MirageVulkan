from pathlib import Path
import re

p = Path(r"C:/MirageWork/MirageVulkan/src/gui/gui_init.cpp")
t = p.read_text(encoding='utf-8', errors='ignore')

# Replace previous stable_sort block (if present) with one that prioritizes X1 by WiFi ADB IP.
pattern = re.compile(r"\n\s*// Force X1 as main in TCP-only mode \(prevents adaptive downscale to 1072\)[\s\S]*?std::stable_sort\(devices\.begin\(\), devices\.end\(\), \[\]\(const auto& a, const auto& b\)\{[\s\S]*?\}\);\n", re.MULTILINE)

new_block = r"\n        // Force X1 as main in TCP-only mode (prevents adaptive downscale to 1072)\n" \
            r"        // Prefer by WiFi ADB endpoint (X1 = 192.168.0.3:5555).\n" \
            r"        std::stable_sort(devices.begin(), devices.end(), [](const auto& a, const auto& b){\n" \
            r"            auto hasX1 = [](const auto& d){\n" \
            r"                for (const auto& w : d.wifi_connections) { if (w.find(\"192.168.0.3:5555\") != std::string::npos) return true; }\n" \
            r"                for (const auto& u : d.usb_connections) { if (u.find(\"93020523431940\") != std::string::npos) return true; }\n" \
            r"                return (d.display_name.find(\"Npad X1\") != std::string::npos);\n" \
            r"            };\n" \
            r"            const bool ax1 = hasX1(a);\n" \
            r"            const bool bx1 = hasX1(b);\n" \
            r"            return (ax1 && !bx1);\n" \
            r"        });\n"

if pattern.search(t):
    t = pattern.sub("\n" + new_block, t, count=1)
else:
    # Fallback: insert right after getUniqueDevices line
    m = re.search(r"auto\s+devices\s*=\s+g_adb_manager->getUniqueDevices\(\);", t)
    if not m:
        raise SystemExit('getUniqueDevices line not found')
    t = t[:m.end()] + new_block + t[m.end():]

p.write_text(t, encoding='utf-8')
print('patched X1 prioritization by ip/serial')
