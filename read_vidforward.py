import os

# AccessoryIoService videoForward (L143-220)
p = r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\usb\AccessoryIoService.kt'
with open(p,'rb') as f: lines = f.read().decode('utf-8','replace').splitlines()
print('=== AccessoryIoService startVideoForward + videoForwardLoop ===')
for i in range(142, min(230, len(lines))): print(f'{i+1}: {lines[i]}')
