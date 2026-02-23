from pathlib import Path
import re
p=Path(r"C:/MirageWork/MirageVulkan/src/gui_application.cpp")
t=p.read_text(encoding='utf-8', errors='ignore')

# Remove existing rotate block before vk_texture->update
rot_pat=re.compile(r"\n\s*// If expected native is portrait but incoming frame is landscape,[\s\S]*?device\.vk_texture->update\(vk_command_pool_, vk_context_->graphicsQueue\(\), rgba_ptr, rw, rh\);\n", re.MULTILINE)
if not rot_pat.search(t):
    raise SystemExit('rotate block not found')

t=rot_pat.sub("\n        device.vk_texture->update(vk_command_pool_, vk_context_->graphicsQueue(), rgba_data, width, height);\n", t, count=1)

# Insert rotate+dim swap earlier, right before transform update comment
anchor='// Update coordinate transform (video->native) for AI/macro/touch mapping.'
idx=t.find(anchor)
if idx==-1:
    raise SystemExit('anchor not found')

insert=r'''// If expected native is portrait but incoming frame is landscape, rotate to keep portrait orientation.
    // Used when encoder falls back to 2000x1200 due to codec max height=1440.
    // IMPORTANT: We swap width/height for ALL subsequent logic (transform/texture/input).
    {
        const bool expectKnown = (device.expected_width > 0 && device.expected_height > 0);
        const bool devicePortrait = expectKnown ? (device.expected_height > device.expected_width) : (height > width);
        const bool frameLandscape = (width > height);
        if (devicePortrait && frameLandscape && rgba_data) {
            static thread_local std::vector<uint8_t> rotate_buf; // reuse per thread
            RotateRgba90CW(rgba_data, width, height, rotate_buf);
            rgba_data = rotate_buf.data();
            std::swap(width, height);
        }
    }

'''

t=t[:idx]+insert+t[idx:]

p.write_text(t, encoding='utf-8')
print('patched rotate dims handling')
