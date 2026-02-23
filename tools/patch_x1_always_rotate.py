from pathlib import Path
import re

p = Path(r"C:/MirageWork/MirageVulkan/src/gui_application.cpp")
t = p.read_text(encoding='utf-8', errors='ignore')

# Patch rotation decision block to treat X1 as portrait even if expected dims are unknown.
start = t.find('// If expected native is portrait but incoming frame is landscape, rotate to keep portrait orientation.')
if start == -1:
    raise SystemExit('rotation block start not found')
end = t.find('// Update coordinate transform (video->native) for AI/macro/touch mapping.', start)
if end == -1:
    raise SystemExit('rotation block end anchor not found')
block = t[start:end]

if 'Always rotate X1' in block:
    print('already patched')
    raise SystemExit(0)

# Replace inner block content between '{' and '}' (the first scope block) with augmented logic.
pat = re.compile(r"\{\s*\n\s*const bool expectKnown[\s\S]*?\n\s*\}\s*\n", re.MULTILINE)
m = pat.search(block)
if not m:
    raise SystemExit('inner rotation scope not found')

new_scope = r'''{
        // Always rotate X1 to portrait if incoming frame is landscape.
        // X1 can be limited by H.264 codec caps; we may end up receiving a landscape frame (e.g. 1800x1080).
        const bool isX1 = (device.display_name.find("Npad X1") != std::string::npos) ||
                          (device.display_name.find("N-one Npad X1") != std::string::npos) ||
                          (device.preferred_adb_id.find("192.168.0.3:5555") != std::string::npos) ||
                          (device.preferred_adb_id.find("93020523431940") != std::string::npos);

        const bool expectKnown = (device.expected_width > 0 && device.expected_height > 0);
        const bool devicePortrait = isX1 ? true : (expectKnown ? (device.expected_height > device.expected_width) : (height > width));
        const bool frameLandscape = (width > height);
        if (devicePortrait && frameLandscape && rgba_data) {
            static thread_local std::vector<uint8_t> rotate_buf; // reuse per thread
            RotateRgba90CW(rgba_data, width, height, rotate_buf);
            rgba_data = rotate_buf.data();
            std::swap(width, height);
            // Mark rotation for input mapping when expected dims are unknown.
            if (!expectKnown && isX1) {
                device.transform.rotation = 90;
            }
        }
    }
'''

block2 = block[:m.start()] + new_scope + block[m.end():]

# Patch rotation inference below: if expected dims unknown but X1 and we rotated, keep rotation=90.
# Replace the else branch that sets rotation=0.
block2 = block2.replace('    } else {\n        device.transform.rotation = 0;\n    }',
                        '    } else {\n        // Always rotate X1 when we received a landscape frame and swapped it above.\n        const bool isX1 = (device.display_name.find("Npad X1") != std::string::npos) ||\n                          (device.display_name.find("N-one Npad X1") != std::string::npos) ||\n                          (device.preferred_adb_id.find("192.168.0.3:5555") != std::string::npos) ||\n                          (device.preferred_adb_id.find("93020523431940") != std::string::npos);\n        device.transform.rotation = (isX1 && device.transform.rotation == 90) ? 90 : 0;\n    }')

# Add marker
block2 = block2.replace('IMPORTANT: We swap width/height for ALL subsequent logic (transform/texture/input).',
                        'IMPORTANT: We swap width/height for ALL subsequent logic (transform/texture/input).\n    // Always rotate X1')

t = t[:start] + block2 + t[end:]

p.write_text(t, encoding='utf-8')
print('patched X1 always-rotate in gui_application.cpp')
