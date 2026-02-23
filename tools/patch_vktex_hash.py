from pathlib import Path
import re
p = Path(r"C:/MirageWork/MirageVulkan/src/vulkan/vulkan_texture.cpp")
t = p.read_text(encoding='utf-8', errors='ignore')

if 'QuickRgbaHash' not in t:
    # add helper near top after includes
    m = re.search(r"(#include[\s\S]*?\n\n)", t)
    if not m:
        raise SystemExit('include block not found')
    ins = r'''
// === Debug: lightweight RGBA hash to detect frozen input ===
static uint64_t QuickRgbaHash(const uint8_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return 0;
    const int sx = 6;
    const int sy = 6;
    uint64_t hash = 1469598103934665603ull;
    for (int y = 0; y < sy; y++) {
        int py = (h - 1) * y / (sy - 1);
        for (int x = 0; x < sx; x++) {
            int px = (w - 1) * x / (sx - 1);
            const uint8_t* p = rgba + (py * w + px) * 4;
            for (int k = 0; k < 4; k++) {
                hash ^= (uint64_t)p[k];
                hash *= 1099511628211ull;
            }
        }
    }
    return hash;
}

'''
    t = t[:m.end(1)] + ins + t[m.end(1):]

# inject logging into VulkanTexture::update
# find the line that logs update#... skip=0
key = 'MLOG_INFO("VkTex", "update#'
idx = t.find(key)
if idx == -1:
    raise SystemExit('update log not found')

# insert after the update log statement end
end = t.find(');', idx)
if end == -1:
    raise SystemExit('cannot find end of update log')
end += 2

if 'InputHash' not in t:
    inject = r'''

    // Debug: hash sampled pixels to confirm input actually changes
    static uint64_t last_hash = 0;
    static int hash_cnt = 0;
    hash_cnt++;
    if (hash_cnt < 20 || (hash_cnt % 300 == 0)) {
        uint64_t h64 = QuickRgbaHash(rgba, width, height);
        MLOG_INFO("VkTex", "InputHash update#%d w=%d h=%d hash=%llu same=%d",\
                  update_count_, width, height, (unsigned long long)h64, (h64 == last_hash));
        last_hash = h64;
    }
'''
    t = t[:end] + inject + t[end:]

p.write_text(t, encoding='utf-8')
print('patched vulkan_texture update with input hash')
