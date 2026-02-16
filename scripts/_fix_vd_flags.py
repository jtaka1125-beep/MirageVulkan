path = r"C:\MirageWork\MirageVulkan\android\MirageStreamer\app\src\main\java\com\mirage\streamer\StreamService.java"
with open(path, "r", encoding="utf-8") as f:
    content = f.read()

# Replace AUTO_MIRROR with PRESENTATION | PUBLIC
# VIRTUAL_DISPLAY_FLAG_PRESENTATION = 1 << 1 = 2
# VIRTUAL_DISPLAY_FLAG_PUBLIC = 1 << 0 = 1  (deprecated but works)
content = content.replace(
    "DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,",
    "DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR\n                    | DisplayManager.VIRTUAL_DISPLAY_FLAG_PRESENTATION,"
)

with open(path, "w", encoding="utf-8") as f:
    f.write(content)
print("PATCHED flags")
