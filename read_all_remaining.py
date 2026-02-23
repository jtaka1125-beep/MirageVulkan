import os, sys

# 全ktファイルを一括で読み込んでまとめて出力
base = r'C:\MirageWork\MirageVulkan\android'
mods = [
    ('capture', 'capture'),
    ('accessory', 'accessory'),
]

# 読みたいファイルリスト（未読優先）
targets = [
    # capture - 未読系
    'UsbVideoSender.kt',
    'UdpVideoSender.kt',
    'TcpVideoSender.kt',
    'VideoSender.kt',
    'AccessoryCommandReceiver.kt',
    'AnnexBSplitter.kt',
    'AnnexB.kt',
    'RtpH264Packetizer.kt',
    'SurfaceRepeater.kt',
    # accessory - 未読系
    'MirageAccessibilityService.kt',
    'DebugCommandReceiver.kt',
    'UdpSender.kt',
]

for mod, pkg in mods:
    src = os.path.join(base, mod, 'src', 'main', 'java')
    for root, dirs, files in os.walk(src):
        for f in files:
            if f in targets:
                p = os.path.join(root, f)
                with open(p, 'rb') as fh:
                    lines = fh.read().decode('utf-8','replace').splitlines()
                print(f'=== [{mod}] {f} ({len(lines)}L) ===')
                for i, l in enumerate(lines, 1):
                    print(f'{i}: {l}')
                print()
