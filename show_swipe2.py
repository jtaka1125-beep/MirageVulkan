
p = r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\usb\AccessoryIoService.kt'
with open(p,'rb') as f: text=f.read().decode('utf-8')
lines = text.splitlines()
for i,l in enumerate(lines,1):
    if 'handleSwipe' in l or 'Swipe' in l:
        print(f'{i}: {repr(l)}')
