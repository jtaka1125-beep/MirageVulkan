from pathlib import Path
import re
p=Path(r"C:/MirageWork/MirageVulkan/src/gui/gui_threads.cpp")
t=p.read_text(encoding='utf-8', errors='ignore')

if 'Force X1 max_size' in t:
    print('already patched')
    raise SystemExit(0)

# Insert after devices fetched
marker = 'auto devices = g_adb_manager->getUniqueDevices();'
idx = t.find(marker)
if idx==-1:
    raise SystemExit('marker not found')

insert = marker + "\n\n        // Force X1 max_size periodically (prevents adaptive downscale to 1072 on TCP-only)\n        for (const auto& dev : devices) {\n            if (dev.display_name.find(\"Npad X1\") != std::string::npos) {\n                const std::string adb_id = !dev.wifi_connections.empty() ? dev.wifi_connections[0] : (!dev.usb_connections.empty() ? dev.usb_connections[0] : std::string());\n                if (!adb_id.empty()) {\n                    g_adb_manager->adbCommand(adb_id, \"shell am broadcast -a com.mirage.capture.ACTION_VIDEO_MAXSIZE -p com.mirage.capture --ei max_size 2000\");\n                    g_adb_manager->adbCommand(adb_id, \"shell am broadcast -a com.mirage.capture.ACTION_VIDEO_IDR -p com.mirage.capture\");\n                    MLOG_INFO(\"watchdog\", \"Force X1 max_size=2000 on %s\", adb_id.c_str());\n                }\n            }\n        }\n"

t = t.replace(marker, insert, 1)

p.write_text(t, encoding='utf-8')
print('patched wifiAdbWatchdogThread to force X1 max_size')
