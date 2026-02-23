p = r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\usb\AccessoryIoService.kt'
with open(p,'rb') as f:
    content = f.read().decode('utf-8')
print(repr(content[content.find('private fun startVideoForward'):content.find('private fun startVideoForward')+200]))
