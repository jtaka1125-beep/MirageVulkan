path = r"C:\MirageWork\MirageVulkan\android\MirageStreamer\app\src\main\java\com\mirage\streamer\StreamService.java"
with open(path, "r", encoding="utf-8") as f:
    content = f.read()

content = content.replace(
    '''                0,  // no flags - most compatible''',
    '''                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR
                    | DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC,'''
)

content = content.replace(
    'Log.i(TAG, "VirtualDisplay created (flags=0) -> streaming!");',
    'Log.i(TAG, "VirtualDisplay created (AUTO_MIRROR|PUBLIC) -> streaming!");'
)

with open(path, "w", encoding="utf-8") as f:
    f.write(content)
print("PATCHED")
