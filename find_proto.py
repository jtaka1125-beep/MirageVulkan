
import sys
p = r'C:\MirageWork\MirageVulkan\src\mirage_protocol.hpp'
with open(p,'rb') as f: raw = f.read()
c = raw.decode('utf-8')
lines = c.splitlines()
for i,l in enumerate(lines,1):
    if 'SWIPE' in l or 'cmd_name' in l or 'PINCH' in l:
        print(f'{i}: {l}')
