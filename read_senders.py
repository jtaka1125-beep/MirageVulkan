import os

# UsbVideoSenderの残り + VideoSender + UdpVideoSender全文
files = [
    r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\capture\UsbVideoSender.kt',
    r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\capture\VideoSender.kt',
    r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\capture\UdpVideoSender.kt',
]
for p in files:
    with open(p,'rb') as f: lines = f.read().decode('utf-8','replace').splitlines()
    print(f'=== {os.path.basename(p)} ({len(lines)}L) ===')
    for i,l in enumerate(lines,1): print(f'{i}: {l}')
    print()
