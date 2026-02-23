
p = r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\usb\AccessoryIoService.kt'
import os
print("size:", os.path.getsize(p))
with open(p, 'rb') as f:
    raw = f.read()
text = raw.decode('utf-8', 'replace')
lines = text.splitlines()
print("total lines:", len(lines))
# print lines 130-250
for i in range(130, min(250, len(lines))):
    print(f'{i+1}: {lines[i]}')
