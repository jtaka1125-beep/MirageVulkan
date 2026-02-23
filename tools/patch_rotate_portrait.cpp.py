from pathlib import Path
import re
p = Path(r"C:/MirageWork/MirageVulkan/src/gui_application.cpp")
t = p.read_text(encoding='utf-8', errors='ignore')

# Add helper rotate near QuickFrameHash
if 'RotateRgba90CW' not in t:
    ins = r'''
// === Rotate RGBA buffer 90deg clockwise (for portrait devices when capture encodes landscape) ===
static void RotateRgba90CW(const uint8_t* src, int sw, int sh, std::vector<uint8_t>& dst) {
    const int dw = sh;
    const int dh = sw;
    dst.resize((size_t)dw * (size_t)dh * 4);
    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            // dst(x',y') where x' = sh-1-y, y' = x
            int dx = sh - 1 - y;
            int dy = x;
            const uint8_t* sp = src + ((size_t)y * sw + x) * 4;
            uint8_t* dp = dst.data() + ((size_t)dy * dw + dx) * 4;
            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3];
        }
    }
}

'''
    # insert after QuickFrameHash helper end
    pos = t.find('return hash;')
    if pos != -1:
        end = t.find('}', pos)
        end = t.find('\n', end) + 1
        t = t[:end] + ins + t[end:]

# In updateDeviceFrame, insert rotation handling before vk_texture->update
if 'rotate_buf' not in t:
    # find the line that calls device.vk_texture->update(...rgba...)
    m = re.search(r"device\.vk_texture->update\(([^\n;]+)\);", t)
    if not m:
        raise SystemExit('vk_texture->update call not found')
    call = m.group(0)
    # insert block just before call
    block = r'''

        // If device is portrait but incoming frame is landscape, rotate to keep portrait orientation.
        // This is used when encoder falls back to 2000x1200 due to codec max height=1440.
        const bool devicePortrait = (device.native_height > device.native_width);
        const bool frameLandscape = (width > height);
        const uint8_t* rgba_ptr = rgba;
        int rw = width;
        int rh = height;
        std::vector<uint8_t> rotate_buf;
        if (devicePortrait && frameLandscape && rgba) {
            RotateRgba90CW(rgba, width, height, rotate_buf);
            rgba_ptr = rotate_buf.data();
            rw = height;
            rh = width;
        }
'''
    t = t.replace(call, block + '\n        ' + call.replace('rgba', 'rgba_ptr').replace('width', 'rw').replace('height', 'rh'))

p.write_text(t, encoding='utf-8')
print('patched gui_application.cpp rotation')
