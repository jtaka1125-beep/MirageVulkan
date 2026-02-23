from pathlib import Path
import re
p=Path(r"C:/MirageWork/MirageVulkan/src/gui/gui_init.cpp")
t=p.read_text(encoding='utf-8', errors='ignore')

# Replace the TCP_ONLY registration loop to register X1 first explicitly
pat=re.compile(r"int wifi_port = mirage::config::getConfig\(\)\.network\.video_base_port;\s*\n\s*bool first = true;\s*\n\s*for \(const auto& dev : devices\) \{\s*\n\s*g_route_controller->registerDevice\(dev\.hardware_id, first, wifi_port\);[\s\S]*?first = false;\s*\n\s*\}\s*\n", re.MULTILINE)

m=pat.search(t)
if not m:
    raise SystemExit('tcp-only registration loop not found')

new = r'''int wifi_port = mirage::config::getConfig().network.video_base_port;
            // Register X1 as main explicitly (avoid order-dependent first flag)
            auto isX1 = [](const auto& d){
                for (const auto& w : d.wifi_connections) { if (w.find("192.168.0.3:5555") != std::string::npos) return true; }
                for (const auto& u : d.usb_connections) { if (u.find("93020523431940") != std::string::npos) return true; }
                return (d.display_name.find("Npad X1") != std::string::npos) || (d.display_name.find("N-one Npad X1") != std::string::npos);
            };

            bool anyX1 = false;
            for (const auto& dev : devices) { if (isX1(dev)) { anyX1 = true; break; } }

            // 1) X1 first (main=1)
            if (anyX1) {
                for (const auto& dev : devices) {
                    if (!isX1(dev)) continue;
                    g_route_controller->registerDevice(dev.hardware_id, true, wifi_port);
                    MLOG_INFO("gui", "RouteController TCP_ONLY: registered %s (%s) main=%d",
                              dev.hardware_id.c_str(), dev.display_name.c_str(), 1);
                    wifi_port++;
                    break;
                }
            }

            // 2) Others (main=0) + also X1 if not found
            bool firstNonX1 = !anyX1;
            for (const auto& dev : devices) {
                if (anyX1 && isX1(dev)) continue;
                const bool is_main = firstNonX1;
                g_route_controller->registerDevice(dev.hardware_id, is_main, wifi_port);
                MLOG_INFO("gui", "RouteController TCP_ONLY: registered %s (%s) main=%d",
                          dev.hardware_id.c_str(), dev.display_name.c_str(), (int)is_main);
                wifi_port++;
                firstNonX1 = false;
            }
'''

t = pat.sub(new, t, count=1)

p.write_text(t, encoding='utf-8')
print('patched tcp-only register loop to force X1 main')
