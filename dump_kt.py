p = r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\capture\ScreenCaptureService.kt'
with open(p,'rb') as f: lines = f.read().decode('utf-8','replace').splitlines()
for i in range(55,200): print(f'{i+1}: {lines[i]}')
