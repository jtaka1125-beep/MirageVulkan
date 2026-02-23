from pathlib import Path
import re
p=Path(r"C:/MirageWork/MirageVulkan/src/gui/gui_init.cpp")
t=p.read_text(encoding='utf-8', errors='ignore')

# Insert sort right after devices fetched in TCP-only registration block
needle = 'auto devices = g_adb_manager->getUniqueDevices();'
# choose the occurrence inside TCP-only block (there are at least two occurrences: fps callback and registration)
idxs=[m.start() for m in re.finditer(re.escape(needle), t)]
if not idxs:
    raise SystemExit('needle not found')

# We want the one that is followed by 'if (!devices.empty())' within next ~200 chars.
target=None
for idx in idxs:
    snippet=t[idx:idx+300]
    if 'if (!devices.empty())' in snippet:
        target=idx
        break
if target is None:
    raise SystemExit('tcp-only devices occurrence not found')

if 'Force X1 as main (TCP_ONLY register)' in t[target:target+400]:
    print('already patched')
    raise SystemExit(0)

insert = needle + "\n            // Force X1 as main (TCP_ONLY register)\n" \
        "            std::stable_sort(devices.begin(), devices.end(), [](const auto& a, const auto& b){\n" \
        "                auto isX1 = [](const auto& d){\n" \
        "                    for (const auto& w : d.wifi_connections) { if (w.find(\"192.168.0.3:5555\") != std::string::npos) return true; }\n" \
        "                    for (const auto& u : d.usb_connections) { if (u.find(\"93020523431940\") != std::string::npos) return true; }\n" \
        "                    return (d.display_name.find(\"Npad X1\") != std::string::npos);\n" \
        "                };\n" \
        "                const bool ax1 = isX1(a);\n" \
        "                const bool bx1 = isX1(b);\n" \
        "                return (ax1 && !bx1);\n" \
        "            });\n"

t = t[:target] + insert + t[target+len(needle):]

p.write_text(t, encoding='utf-8')
print('patched tcp-only registration sort')
