from pathlib import Path
import re

p = Path(r"C:/MirageWork/MirageVulkan/src/gui_application.cpp")
t = p.read_text(encoding='utf-8', errors='ignore')

# 1) Insert RotateRgba90CW helper after namespace opening if missing
if 'static void RotateRgba90CW' not in t:
    ns = t.find('namespace mirage::gui {')
    if ns == -1:
        raise SystemExit('namespace not found')
    ins_pos = t.find('\n', ns) + 1
    helper = r'''
// === Rotate RGBA buffer 90deg clockwise (portrait view for landscape frames) ===
static void RotateRgba90CW(const uint8_t* src, int sw, int sh, std::vector<uint8_t>& dst) {
    const int dw = sh;
    const int dh = sw;
    dst.resize((size_t)dw * (size_t)dh * 4);
    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            int dx = sh - 1 - y;
            int dy = x;
            const uint8_t* sp = src + ((size_t)y * sw + x) * 4;
            uint8_t* dp = dst.data() + ((size_t)dy * dw + dx) * 4;
            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3];
        }
    }
}

'''
    t = t[:ins_pos] + helper + t[ins_pos:]

# 2) In updateDeviceFrame, define rgba_ptr and rotate for X1 id
# Add rgba_ptr right after DeviceInfo& device = it->second;
marker = 'DeviceInfo& device = it->second;'
if marker not in t:
    raise SystemExit('device marker not found')
if 'rgba_ptr' not in t[t.find(marker):t.find(marker)+200]:
    t = t.replace(marker, marker + '\n\n    // Allow rotation / re-mapping without touching the original pointer\n    const uint8_t* rgba_ptr = rgba_data;\n', 1)

# Insert rotation block just before transform update comment
anchor = '// Update coordinate transform (video->native) for AI/macro/touch mapping.'
idx = t.find(anchor)
if idx == -1:
    raise SystemExit('transform anchor not found')
if 'Force portrait rotate for X1' not in t[idx-400:idx]:
    rot_block = r'''    // Force portrait rotate for X1 when incoming frame is landscape.
    // X1 H.264 codec often forces landscape (e.g. 1800x1080). We always display it portrait.
    {
        const bool isX1 = (id.find("f1925da3_") != std::string::npos);
        const bool frameLandscape = (width > height);
        if (isX1 && frameLandscape && rgba_ptr) {
            static thread_local std::vector<uint8_t> rotate_buf;
            RotateRgba90CW(rgba_ptr, width, height, rotate_buf);
            rgba_ptr = rotate_buf.data();
            std::swap(width, height);
            // Inform input mapping that we're rotated.
            device.transform.rotation = 90;
        }
    }

'''
    t = t[:idx] + rot_block + t[idx:]

# 3) Use rgba_ptr for texture update
# Replace the update call line
t = t.replace('device.vk_texture->update(vk_command_pool_, vk_context_->graphicsQueue(), rgba_data, width, height);',
              'device.vk_texture->update(vk_command_pool_, vk_context_->graphicsQueue(), rgba_ptr, width, height);')

p.write_text(t, encoding='utf-8')
print('patched X1 always rotate + rgba_ptr')
