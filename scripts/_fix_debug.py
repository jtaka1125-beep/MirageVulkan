path = r"C:\MirageWork\MirageVulkan\android\MirageStreamer\app\src\main\java\com\mirage\streamer\StreamService.java"
with open(path, "r", encoding="utf-8") as f:
    content = f.read()

# Revert the flag change
content = content.replace(
    "DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR\n                    | DisplayManager.VIRTUAL_DISPLAY_FLAG_PRESENTATION,",
    "DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,"
)

# Add debug logging in encoder loop
old_loop = '''                int index = encoder.dequeueOutputBuffer(info, 10_000); // 10ms timeout
                if (index >= 0) {'''
new_loop = '''                int index = encoder.dequeueOutputBuffer(info, 1_000_000); // 1s timeout
                if (frameCount == 0 && index < 0) {
                    Log.d(TAG, "dequeueOutputBuffer returned " + index);
                }
                if (index >= 0) {'''

content = content.replace(old_loop, new_loop)

with open(path, "w", encoding="utf-8") as f:
    f.write(content)
print("PATCHED debug")
