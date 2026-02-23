import os

# 重要ファイルを個別に読む
files_priority = [
    (r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\capture\UsbVideoSender.kt', 'UsbVideoSender'),
    (r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\capture\VideoSender.kt', 'VideoSender'),
    (r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\capture\UdpVideoSender.kt', 'UdpVideoSender'),
    (r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\capture\TcpVideoSender.kt', 'TcpVideoSender'),
    (r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\ipc\AccessoryCommandReceiver.kt', 'AccessoryCommandReceiver'),
    (r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\access\MirageAccessibilityService.kt', 'MirageAccessibilityService'),
    (r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\capture\RtpH264Packetizer.kt', 'RtpH264Packetizer'),
]

for p, name in files_priority:
    with open(p, 'rb') as f: lines = f.read().decode('utf-8','replace').splitlines()
    print(f'=== {name} ({len(lines)}L) ===')
    for i, l in enumerate(lines, 1): print(f'{i}: {l}')
    print()
