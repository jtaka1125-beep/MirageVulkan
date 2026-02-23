
p = r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\usb\AccessoryIoService.kt'
with open(p,'rb') as f: text=f.read().decode('utf-8')
lines = text.splitlines()
for i in range(400, 410):
    print(f'{i+1}: {repr(lines[i])}')
