
p = r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\usb\AccessoryIoService.kt'
with open(p,'rb') as f: raw = f.read()
content = raw.decode('utf-8')

# Use the actual file content (CRLF)
old_start = content.find('    private fun startVideoForward() {')
old_end = content.find('    private fun stopVideoForward() {')
old = content[old_start:old_end].rstrip()

print(f"old_start={old_start} old_end={old_end}")
print(f"old length={len(old)}")
print(repr(old[:120]))
