
import os
files = [
    r'C:\MirageWork\MirageVulkan\src\aoa_hid_touch.hpp',
    r'C:\MirageWork\MirageVulkan\src\multi_usb_command_sender.cpp',
]
for p in files:
    with open(p,'rb') as f: c=f.read().decode('utf-8')
    print(f'\n=== {os.path.basename(p)}: {os.path.getsize(p)} bytes, {len(c.splitlines())} lines ===')
    for i,l in enumerate(c.splitlines(),1):
        if any(x in l.lower() for x in ['pinch','angle','long_press','longpress','_pending','toctou','ewma','smoothing']):
            print(f'  {i}: {l}')
