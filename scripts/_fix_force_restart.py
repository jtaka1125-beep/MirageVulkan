path = r"C:\MirageWork\MirageVulkan\src\auto_setup.hpp"
with open(path, "r", encoding="utf-8") as f:
    content = f.read()

# route_mode 0 (USB) doesn't work because switchSender returns null for USB.
# Use route_mode 2 (TCP) instead to force a real mode change.
# AccessoryCommandReceiver maps: 0=USB, 1=UDP, else=UDP
# But ScreenCaptureService.switchSender accepts "tcp" directly.
# Problem: AccessoryCommandReceiver only maps 0->usb, else->udp
# So we need a different approach: just send 2 UDP broadcasts with different port to force re-create

old = '''            // Step 1: Switch to USB/TCP mode to force sender reset
            std::string cmd_reset = "shell am broadcast"
                " -a com.mirage.capture.ACTION_VIDEO_ROUTE"
                " --ei route_mode 0"
                " -n com.mirage.capture/.ipc.AccessoryCommandReceiver";
            adb_executor_(cmd_reset);

            // Brief delay for mode switch
            std::this_thread::sleep_for(std::chrono::milliseconds(300));

            // Step 2: Switch to UDP mode with correct host:port'''

new = '''            // Force sender re-creation by switching to dummy UDP first, then real target.
            // switchSender skips if mode==mirrorMode, so use IDR to flush, then
            // kill the service and restart it won't work without MediaProjection.
            // Instead: directly broadcast the target - if already UDP with different host/port,
            // the switchSender's mode check will skip it.
            // Workaround: stop current capture via force-stop, then let watchdog restart.
            // Actually simplest: just send the route command - if the sender was created
            // with wrong host (from previous session), this won't help.
            //
            // Real fix: force-stop the capture app, then have CaptureActivity auto-start.
            adb_executor_("shell am force-stop com.mirage.capture");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            // Launch CaptureActivity which will get MediaProjection and start service
            std::string cmd_activity = "shell am start"
                " -n com.mirage.capture/.ui.CaptureActivity"
                " --es host " + host +
                " --ei port " + std::to_string(port);
            adb_executor_(cmd_activity);'''

content = content.replace(old, new, 1)

# Also remove the step 2 broadcast since we're using activity launch now
old2 = '''            // Step 2: Switch to UDP mode with correct host:port
            std::string cmd_udp = "shell am broadcast"
                " -a com.mirage.capture.ACTION_VIDEO_ROUTE"
                " --ei route_mode 1"
                " --es host " + host +
                " --ei port " + std::to_string(port) +
                " -n com.mirage.capture/.ipc.AccessoryCommandReceiver";
            adb_executor_(cmd_udp);'''

new2 = '''            // CaptureActivity will prompt for screen capture permission,
            // then start ScreenCaptureService with correct host:port.
            // approve_screen_share_dialog() handles the permission tap.'''

content = content.replace(old2, new2, 1)

with open(path, "w", encoding="utf-8") as f:
    f.write(content)
print("PATCHED")
