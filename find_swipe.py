
p = r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\usb\Protocol.kt'
with open(p,'rb') as f: raw=f.read()
c = raw.decode('utf-8')
lines = c.splitlines()
for i,l in enumerate(lines,1):
    if 'SWIPE' in l or 'Swipe' in l:
        print(f'{i}: {repr(l)}')
