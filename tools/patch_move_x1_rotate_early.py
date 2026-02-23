from pathlib import Path
import re
p=Path(r"C:/MirageWork/MirageVulkan/src/gui_application.cpp")
t=p.read_text(encoding='utf-8', errors='ignore')

# Remove existing late rotation block
late_pat=re.compile(r"\n\s*// Force portrait rotate for X1 when incoming frame is landscape\.[\s\S]*?\n\s*\}\n\n", re.MULTILINE)
t2, n = late_pat.subn('\n', t, count=1)
if n==0:
    raise SystemExit('late rotation block not found')
t=t2

# Insert early rotation block right after rgba_ptr declaration
marker = 'const uint8_t* rgba_ptr = rgba_data;'
idx = t.find(marker)
if idx==-1:
    raise SystemExit('rgba_ptr marker not found')
ins_pos = t.find('\n', idx)+1

early = r'''
    bool x1_rotated = false;
    // Force portrait rotate for X1 when incoming frame is landscape.
    // Must happen BEFORE expected-resolution checks so frames are not rejected.
    {
        const bool isX1 = (id.find("f1925da3_") != std::string::npos);
        const bool frameLandscape = (width > height);
        if (isX1 && frameLandscape && rgba_ptr) {
            static thread_local std::vector<uint8_t> rotate_buf;
            RotateRgba90CW(rgba_ptr, width, height, rotate_buf);
            rgba_ptr = rotate_buf.data();
            std::swap(width, height);
            x1_rotated = true;
        }
    }
'''

if 'x1_rotated' not in t[ins_pos:ins_pos+400]:
    t = t[:ins_pos] + early + t[ins_pos:]

# After transform.rotation inference, force 90 when x1_rotated
force_pat = re.compile(r"device\.transform\.rotation = 0;\n\s*\}")
# We'll insert after device.transform.recalculate(); maybe safer
rec_marker = 'device.transform.recalculate();'
rec_idx = t.find(rec_marker)
if rec_idx==-1:
    raise SystemExit('recalculate marker not found')
rec_line_end = t.find('\n', rec_idx)+1
if 'if (x1_rotated)' not in t[rec_line_end:rec_line_end+200]:
    t = t[:rec_line_end] + '    if (x1_rotated) device.transform.rotation = 90;\n    device.transform.recalculate();\n' + t[rec_line_end:]
    # remove the old recalculate line we duplicated
    t = t.replace('device.transform.recalculate();\n    if (x1_rotated) device.transform.rotation = 90;\n    device.transform.recalculate();\n',
                  'if (x1_rotated) device.transform.rotation = 90;\n    device.transform.recalculate();\n', 1)

p.write_text(t, encoding='utf-8')
print('moved X1 rotation early and forced rotation=90')
