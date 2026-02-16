path = r"C:\MirageWork\MirageVulkan\src\auto_setup.hpp"
with open(path, "r", encoding="utf-8") as f:
    content = f.read()

old = '''    SetupStepResult approve_screen_share_dialog() {
        if (progress_callback_) {
            progress_callback_("Approving screen share dialog...", 50);
        }
        if (adb_executor_) {
            // Tap "Start now" button (common coordinates)
            adb_executor_("shell input tap 540 1150");
        }'''

new = '''    SetupStepResult approve_screen_share_dialog() {
        // No longer needed: broadcast-based route switch doesn't trigger dialog.
        // Kept for API compatibility.'''

if old in content:
    content = content.replace(old, new, 1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
    print("PATCHED")
else:
    print("NOT FOUND")
