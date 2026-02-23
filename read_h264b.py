p = r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\capture\H264Encoder.kt'
with open(p,'rb') as f: lines = f.read().decode('utf-8','replace').splitlines()
# L82-195 (start/stop/encoderLoop先頭)
for i in range(81, min(200, len(lines))): print(f'{i+1}: {lines[i]}')
