from pathlib import Path
import re
p=Path(r"C:/MirageWork/MirageVulkan/src/gui/gui_init.cpp")
t=p.read_text(encoding='utf-8', errors='ignore')

# Ensure ACTION_VIDEO_FPS
if 'com.mirage.capture.SET_FPS' in t:
    t=t.replace('com.mirage.capture.SET_FPS','com.mirage.capture.ACTION_VIDEO_FPS')

# Patch the TCP_ONLY registration loop only (the one that logs "RouteController TCP_ONLY: registered")
pat=re.compile(
    r"int wifi_port = mirage::config::getConfig\(\)\.network\.video_base_port;\s*\n\s*bool first = true;\s*\n\s*for \(const auto& dev : devices\) \{\s*\n\s*g_route_controller->registerDevice\(dev\.hardware_id, first, wifi_port\);\s*\n\s*MLOG_INFO\(\"gui\", \"RouteController TCP_ONLY: registered %s \(%s\) main=%d\",\s*\n\s*dev\.hardware_id\.c_str\(\), dev\.display_name\.c_str\(\), first\);\s*\n\s*wifi_port\+\+;\s*\n\s*first = false;\s*\n\s*\}\s*",
    re.MULTILINE)

m=pat.search(t)
if not m:
    raise SystemExit('tcp-only loop pattern not found (file changed?)')

new=r'''int wifi_port = mirage::config::getConfig().network.video_base_port;
            // Choose main device explicitly: prefer X1 (192.168.0.3:5555 / 93020523431940 / name match)
            auto isX1 = [](const auto& dev){
                if (dev.preferred_adb_id.find("192.168.0.3:5555") != std::string::npos) return true;
                for (const auto& w : dev.wifi_connections) { if (w.find("192.168.0.3:5555") != std::string::npos) return true; }
                for (const auto& u : dev.usb_connections) { if (u.find("93020523431940") != std::string::npos) return true; }
                return (dev.display_name.find("Npad X1") != std::string::npos) || (dev.display_name.find("N-one Npad X1") != std::string::npos);
            };

            bool hasX1 = false;
            for (const auto& dev : devices) { if (isX1(dev)) { hasX1 = true; break; } }

            bool first = true;
            for (const auto& dev : devices) {
                const bool mainFlag = hasX1 ? isX1(dev) : first;
                g_route_controller->registerDevice(dev.hardware_id, mainFlag, wifi_port);
                MLOG_INFO("gui", "RouteController TCP_ONLY: registered %s (%s) main=%d",
                          dev.hardware_id.c_str(), dev.display_name.c_str(), (int)mainFlag);
                wifi_port++;
                first = false;
            }'''

t=t[:m.start()]+new+t[m.end():]

p.write_text(t, encoding='utf-8')
print('patched tcp-only main selection + ACTION_VIDEO_FPS')
