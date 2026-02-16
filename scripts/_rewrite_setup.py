path = r"C:\MirageWork\MirageVulkan\src\auto_setup.hpp"
with open(path, "r", encoding="utf-8") as f:
    lines = f.readlines()

# Find and replace the start_screen_capture method
new_content = []
skip = False
for i, line in enumerate(lines):
    if 'SetupStepResult start_screen_capture' in line:
        skip = True
        new_content.append("""    SetupStepResult start_screen_capture(const std::string& host, int port) {
        if (progress_callback_) {
            progress_callback_("Starting screen capture...", 25);
        }
        if (adb_executor_) {
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
        }
        SetupStepResult result;
        result.status = SetupStatus::COMPLETED;
        return result;
    }

""")
        continue
    if skip:
        if line.strip() == '}' and i + 1 < len(lines) and 'SetupStepResult approve' in lines[i+1]:
            skip = False
        continue
    
    # Fix approve_screen_share_dialog to tap the permission dialog
    if 'SetupStepResult approve_screen_share_dialog' in line:
        skip = True
        new_content.append("""    SetupStepResult approve_screen_share_dialog() {
        if (progress_callback_) {
            progress_callback_("Approving screen share dialog...", 50);
        }
        if (adb_executor_) {
            // Wait for MediaProjection permission dialog, then tap "Start now"
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            adb_executor_("shell input tap 540 1150");
        }
        SetupStepResult result;
        result.status = SetupStatus::COMPLETED;
        return result;
    }

""")
        continue
    if skip:
        if line.strip() == '}' and i + 1 < len(lines) and 'SetupStepResult' in lines[i+1]:
            skip = False
        continue

    new_content.append(line)

with open(path, "w", encoding="utf-8") as f:
    f.writelines(new_content)
print("REWRITTEN")
