
p = r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\usb\Protocol.kt'
with open(p,'rb') as f: raw=f.read()
c = raw.decode('utf-8')
lines = c.splitlines()
for i,l in enumerate(lines,1):
    if 'PINCH' in l or 'LONGPRESS' in l or 'LongPress' in l or 'Pinch' in l:
        print(f'{i}: {l}')
