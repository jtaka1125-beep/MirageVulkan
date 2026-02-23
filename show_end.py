
p = r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\usb\AccessoryIoService.kt'
with open(p, 'rb') as f:
    raw = f.read()
text = raw.decode('utf-8', 'replace')
lines = text.splitlines()
print("total lines:", len(lines))
# print lines 280-430
for i in range(280, len(lines)):
    print(f'{i+1}: {lines[i]}')
