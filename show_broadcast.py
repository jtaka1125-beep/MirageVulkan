
p = r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\usb\AccessoryIoService.kt'
with open(p,'rb') as f: raw=f.read()
c = raw.decode('utf-8')
lines = c.splitlines()
# Show lines 700-840
for i in range(699, min(840, len(lines))):
    print(f'{i+1}: {lines[i]}')
