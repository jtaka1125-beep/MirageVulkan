path = r"C:\MirageWork\MirageVulkan\src\auto_setup.hpp"
with open(path, "r", encoding="utf-8") as f:
    content = f.read()

old = '''        if (adb_executor_) {
            // Force-stop to get fresh MediaProjection
            adb_executor_("shell am force-stop com.mirage.capture");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            // Launch CaptureActivity with auto_mirror flag
            // CaptureActivity.handleAutoStart() reads mirror_host/mirror_port
            std::string cmd = "shell am start"
                " -n com.mirage.capture/.ui.CaptureActivity"
                " --ez auto_mirror true"
                " --es mirror_mode udp"
                " --es mirror_host " + host +
                " --ei mirror_port " + std::to_string(port);
            adb_executor_(cmd);
        }'''

new = '''        if (adb_executor_) {
            // Force-stop to get fresh state
            adb_executor_("shell am force-stop com.mirage.streamer");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            // Launch MirageStreamer: MediaProjection -> H.264 -> UDP
            std::string cmd = "shell am start"
                " -n com.mirage.streamer/.StreamActivity"
                " --es host " + host +
                " --ei port " + std::to_string(port);
            adb_executor_(cmd);
        }'''

if old in content:
    content = content.replace(old, new, 1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
    print("PATCHED")
else:
    print("NOT FOUND")
