import os, difflib

paths = {
    'app':       r'C:\MirageWork\MirageVulkan\android\app\src\main\java\com\mirage\android\usb\Protocol.kt',
    'capture':   r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\usb\Protocol.kt',
    'accessory': r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\usb\Protocol.kt',
}
texts = {}
for k, p in paths.items():
    with open(p,'rb') as f: texts[k] = f.read().decode('utf-8','replace').splitlines()

pairs = [('app','capture'),('capture','accessory'),('app','accessory')]
print('=== Protocol.kt diff ===')
for a,b in pairs:
    diff = list(difflib.unified_diff(texts[a], texts[b], lineterm=''))
    adds = sum(1 for l in diff if l.startswith('+') and not l.startswith('+++'))
    dels = sum(1 for l in diff if l.startswith('-') and not l.startswith('---'))
    print(f'{a} vs {b}: +{adds} -{dels} (lines: {len(texts[a])} vs {len(texts[b])})')

# 実際の差分内容
print('\n--- capture vs accessory diff (meaningful lines) ---')
diff = list(difflib.unified_diff(texts['capture'], texts['accessory'], lineterm='', n=0))
for l in diff[:80]: print(l)

# 同名ファイルを全モジュールで比較
same_names = ['H264Encoder.kt','AnnexBSplitter.kt','RtpH264Packetizer.kt','VideoSender.kt','UdpVideoSender.kt','IntentCompat.kt','VidMeta.kt']
print('\n=== 同名ファイル行数比較 ===')
for name in same_names:
    row = []
    for mod, pkg in [('app','android'),('capture','capture')]:
        base = rf'C:\MirageWork\MirageVulkan\android\{mod}\src\main\java\com\mirage\{pkg}'
        for root, dirs, files in os.walk(base):
            for f in files:
                if f == name:
                    p = os.path.join(root, f)
                    with open(p,'rb') as fh: n = len(fh.read().decode('utf-8','replace').splitlines())
                    row.append(f'{mod}:{n}')
    print(f'  {name}: {", ".join(row)}')
