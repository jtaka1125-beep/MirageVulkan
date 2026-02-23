p = r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\ui\CaptureActivity.kt'
with open(p,'rb') as f: lines = f.read().decode('utf-8','replace').splitlines()
for i, l in enumerate(lines, 1): print(f'{i}: {l}')
