path = r"C:\MirageWork\MirageVulkan\android\MirageStreamer\app\src\main\java\com\mirage\streamer\StreamService.java"
with open(path, "r", encoding="utf-8") as f:
    content = f.read()

# Fix density and add import
content = content.replace(
    "import android.view.Surface;",
    "import android.util.DisplayMetrics;\nimport android.view.Surface;\nimport android.view.WindowManager;"
)

# Replace VirtualDisplay creation with dynamic density
content = content.replace(
    '''        virtualDisplay = projection.createVirtualDisplay(
                "MirageStreamer",
                width, height, 1,  // density=1
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
                inputSurface, null, null);''',
    '''        // Get actual screen density
        DisplayMetrics dm = new DisplayMetrics();
        ((WindowManager) getSystemService(WINDOW_SERVICE)).getDefaultDisplay().getMetrics(dm);
        int dpi = dm.densityDpi;
        Log.i(TAG, "Screen density: " + dpi + " dpi");

        virtualDisplay = projection.createVirtualDisplay(
                "MirageStreamer",
                width, height, dpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
                inputSurface, null, null);'''
)

with open(path, "w", encoding="utf-8") as f:
    f.write(content)
print("PATCHED density")
