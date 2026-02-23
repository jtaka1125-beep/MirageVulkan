from pathlib import Path
import re

p = Path(r"C:/MirageWork/MirageVulkan/src/gui_application.cpp")
t = p.read_text(encoding='utf-8', errors='ignore')

# Find the rotation block comment we added earlier
start = t.find('// If expected native is portrait but incoming frame is landscape, rotate to keep portrait orientation.')
if start == -1:
    raise SystemExit('rotation block start not found')
end = t.find('// Update coordinate transform (video->native) for AI/macro/touch mapping.', start)
if end == -1:
    raise SystemExit('rotation block end anchor not found')
block = t[start:end]

# Patch devicePortrait definition line to include X1 by id
# existing line:
# const bool devicePortrait = expectKnown ? (...) : (height > width);
pat = re.compile(r"const bool devicePortrait = expectKnown \? \(device\.expected_height > device\.expected_width\) : \(height > width\);")
if not pat.search(block):
    raise SystemExit('devicePortrait line not found')

# Add isX1 and modify devicePortrait + rotation flag assignment.
# We'll insert isX1 right after frameLandscape.
if 'isX1_by_id' not in block:
    block = block.replace('const bool frameLandscape = (width > height);',
                          'const bool frameLandscape = (width > height);\n        const bool isX1_by_id = (id.find("f1925da3_") != std::string::npos);')

block = pat.sub('const bool devicePortrait = isX1_by_id ? true : (expectKnown ? (device.expected_height > device.expected_width) : (height > width));', block, count=1)

# Ensure we set transform.rotation=90 for X1 when we rotate
# Find inside if (devicePortrait && frameLandscape && rgba_data) { ... swap ... }
# Insert after swap
insert_pat = re.compile(r"std::swap\(width, height\);")
if not insert_pat.search(block):
    raise SystemExit('swap line not found')
block = insert_pat.sub('std::swap(width, height);\n            if (isX1_by_id) { device.transform.rotation = 90; }', block, count=1)

# In the rotation inference section, keep rotation=90 for X1 even when expected dims unknown.
block = block.replace('} else {\n        device.transform.rotation = 0;\n    }',
                      '} else {\n        const bool isX1_by_id = (id.find("f1925da3_") != std::string::npos);\n        device.transform.rotation = isX1_by_id ? 90 : 0;\n    }')

# Write back
p.write_text(t[:start] + block + t[end:], encoding='utf-8')
print('patched X1 force rotate by id')
