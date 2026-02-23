
p = r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\usb\AccessoryIoService.kt'
with open(p,'rb') as f: raw=f.read()
c = raw.decode('utf-8')
lines = c.splitlines()
for i,l in enumerate(lines,1):
    if 'handlePinch' in l or 'handleLongPress' in l or 'handleSwipe' in l or 'handleClick' in l or 'FIX-A' in l or 'FIX-B' in l:
        print(f'{i}: {l}')
