
p = r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\usb\AccessoryIoService.kt'
with open(p,'rb') as f: raw=f.read()
c = raw.decode('utf-8')
lines = c.splitlines()
# Find broadcastCommand / when(cmd) block
for i,l in enumerate(lines,1):
    if 'CMD_SWIPE' in l or 'broadcastCommand' in l or 'CMD_BACK' in l or 'CMD_CLICK' in l:
        print(f'{i}: {l}')
