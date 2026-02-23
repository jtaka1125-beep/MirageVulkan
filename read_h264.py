import os

# H264Encoder全文 (capture)
p = r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\capture\H264Encoder.kt'
with open(p,'rb') as f: lines = f.read().decode('utf-8','replace').splitlines()
for i,l in enumerate(lines,1): print(f'{i}: {l}')
