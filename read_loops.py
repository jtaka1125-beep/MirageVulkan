import os

# H264Encoder sendLoop部分 (L200-280)
p = r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\capture\H264Encoder.kt'
with open(p,'rb') as f: lines = f.read().decode('utf-8','replace').splitlines()
print(f'=== H264Encoder sendLoop L200-280 ===')
for i in range(199, min(280, len(lines))): print(f'{i+1}: {lines[i]}')

print()

# AccessoryIoService videoForward部分 (L143-220)
p2 = r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\usb\AccessoryIoService.kt'
with open(p2,'rb') as f: lines2 = f.read().decode('utf-8','replace').splitlines()
print(f'=== AccessoryIoService L143-222 ===')
for i in range(142, min(222, len(lines2))): print(f'{i+1}: {lines2[i]}')

print()

# TcpVideoSender acceptLoop/writeLoop (L85-180)
p3 = r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\capture\TcpVideoSender.kt'
with open(p3,'rb') as f: lines3 = f.read().decode('utf-8','replace').splitlines()
print(f'=== TcpVideoSender L85-185 ===')
for i in range(84, min(185, len(lines3))): print(f'{i+1}: {lines3[i]}')
