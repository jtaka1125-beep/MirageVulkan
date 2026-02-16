path = r"C:\MirageWork\MirageVulkan\src\auto_setup.hpp"
with open(path, "r", encoding="utf-8") as f:
    content = f.read()

old = '''            std::string cmd = "shell am start-foreground-service -n com.mirage.capture/.capture.ScreenCaptureService "
                             "--es HOST " + host + " --ei PORT " + std::to_string(port);
            adb_executor_(cmd);'''

new = '''            // ScreenCaptureService already running (started by CaptureActivity).
            // Use broadcast to switch video route via AccessoryCommandReceiver.
            // Step 1: Switch to USB/TCP mode to force sender reset
            std::string cmd_reset = "shell am broadcast"
                " -a com.mirage.capture.ACTION_VIDEO_ROUTE"
                " --ei route_mode 0"
                " -n com.mirage.capture/.ipc.AccessoryCommandReceiver";
            adb_executor_(cmd_reset);

            // Brief delay for mode switch
            std::this_thread::sleep_for(std::chrono::milliseconds(300));

            // Step 2: Switch to UDP mode with correct host:port
            std::string cmd_udp = "shell am broadcast"
                " -a com.mirage.capture.ACTION_VIDEO_ROUTE"
                " --ei route_mode 1"
                " --es host " + host +
                " --ei port " + std::to_string(port) +
                " -n com.mirage.capture/.ipc.AccessoryCommandReceiver";
            adb_executor_(cmd_udp);'''

if old in content:
    content = content.replace(old, new, 1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
    print("PATCHED")
else:
    print("NOT FOUND")
