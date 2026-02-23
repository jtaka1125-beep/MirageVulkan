from pathlib import Path
import re
p=Path(r"C:/MirageWork/MirageVulkan/src/gui/gui_init.cpp")
t=p.read_text(encoding='utf-8', errors='ignore')

# Patch TCP_ONLY registration loop: replace 'first' logic with name-based main selection.
# Find the block that declares bool first = true; and calls registerDevice(..., first, wifi_port)
if 'main_by_name' in t:
    print('already patched')
    raise SystemExit(0)

# Replace the two lines around registerDevice
# We'll locate 'bool first = true;' then within the for-loop find registerDevice call.
# We'll inject: bool isMain = dev.display_name.find("Npad X1") != std::string::npos;
# and use (isMain || first) for main.

t = t.replace('bool first = true;', 'bool first = true; // fallback if no X1 found\n    bool foundX1 = false; // main_by_name')

# Replace registerDevice line
reg_pat = re.compile(r"g_route_controller->registerDevice\(dev\.hardware_id,\s*first,\s*wifi_port\);")
if not reg_pat.search(t):
    raise SystemExit('registerDevice(dev.hardware_id, first, wifi_port) not found')

t = reg_pat.sub('const bool isMain = (dev.display_name.find("Npad X1") != std::string::npos);\n        if (isMain) foundX1 = true;\n        g_route_controller->registerDevice(dev.hardware_id, (isMain || (!foundX1 && first)), wifi_port);', t, count=1)

# Ensure first toggles stays
p.write_text(t, encoding='utf-8')
print('patched TCP_ONLY main selection by display_name')
