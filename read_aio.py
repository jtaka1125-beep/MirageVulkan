import os

# AccessoryIoService 全文
p = r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\usb\AccessoryIoService.kt'
with open(p,'rb') as f: lines = f.read().decode('utf-8','replace').splitlines()
for i,l in enumerate(lines,1): print(f'{i}: {l}')
