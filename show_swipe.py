
p = r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\usb\Protocol.kt'
with open(p,'rb') as f: raw=f.read()
c = raw.decode('utf-8')
lines = c.splitlines()
# show lines 175-195
for i in range(174, min(196, len(lines))):
    print(f'{i+1}: {repr(lines[i])}')
