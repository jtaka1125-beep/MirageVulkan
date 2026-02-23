
p = r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\usb\AccessoryIoService.kt'
with open(p,'rb') as f: raw = f.read()
content = raw.decode('utf-8')
idx = content.find('private fun startVideoForward')
print(content[idx:idx+1600])
