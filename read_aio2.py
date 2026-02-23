p = r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\usb\AccessoryIoService.kt'
with open(p,'rb') as f: lines = f.read().decode('utf-8','replace').splitlines()
# L74-260 (core IO logic)
for i in range(73, min(260, len(lines))): print(f'{i+1}: {lines[i]}')
