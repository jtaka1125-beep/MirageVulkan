p = r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\usb\AccessoryIoService.kt'
with open(p,'rb') as f: lines = f.read().decode('utf-8','replace').splitlines()
# ioLoop, videoForward, parseCommand core
for i in range(260, min(370, len(lines))): print(f'{i+1}: {lines[i]}')
