from pathlib import Path
import re

p = Path(r"C:/MirageWork/MirageVulkan/src/gui_application.cpp")
t = p.read_text(encoding='utf-8', errors='ignore')

if 'QuickFrameHash' not in t:
    # Insert helper near top (after includes)
    m = re.search(r"(#include \"imgui_impl_vulkan\.h\"[\s\S]*?\n)", t)
    if not m:
        # fallback: after first include block
        m = re.search(r"(#include[\s\S]*?\n\n)", t)
    if m:
        insert_at = m.end(1)
        helper = r'''
// === Debug: lightweight frame hash to detect stale frames ===
static uint64_t QuickFrameHash(const uint8_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return 0;
    // Sample a grid of pixels to avoid full scan
    const int sx = 8;
    const int sy = 8;
    uint64_t hash = 1469598103934665603ull; // FNV offset basis
    for (int y = 0; y < sy; y++) {
        int py = (h - 1) * y / (sy - 1);
        for (int x = 0; x < sx; x++) {
            int px = (w - 1) * x / (sx - 1);
            const uint8_t* p = rgba + (py * w + px) * 4;
            // FNV-1a over 4 bytes
            for (int k = 0; k < 4; k++) {
                hash ^= (uint64_t)p[k];
                hash *= 1099511628211ull;
            }
        }
    }
    return hash;
}
'''
        t = t[:insert_at] + helper + t[insert_at:]

# Patch inside GuiApplication::updateDeviceFrame: after existing updateDeviceFrame log, add hash log every 300 calls
if 'FrameHash' not in t:
    # locate the main log line
    key = 'MLOG_INFO("app", "[updateDeviceFrame]'
    idx = t.find(key)
    if idx != -1:
        # find end of that log statement (next ';')
        end = t.find(');', idx)
        if end != -1:
            end += 2
            inject = r'''

    // Debug: detect stale content (hash of sampled pixels)
    static std::map<std::string, uint64_t> last_hash;
    static std::map<std::string, int> hash_count;
    int& hc = hash_count[id];
    hc++;
    if (hc < 20 || (hc % 300 == 0)) {
        uint64_t h64 = QuickFrameHash(rgba, width, height);
        uint64_t prev = last_hash[id];
        last_hash[id] = h64;
        MLOG_INFO("app", "[FrameHash] device=%s w=%d h=%d hash=%llu prev=%llu same=%d",\
                  id.c_str(), width, height, (unsigned long long)h64, (unsigned long long)prev, (h64==prev));
    }
'''
            t = t[:end] + inject + t[end:]

p.write_text(t, encoding='utf-8')
print('patched frame hash probe')
