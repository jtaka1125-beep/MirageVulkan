import os

# MirageAccessibilityService L63-200 (onAccessibilityEvent + swipe/pinch/longpress)
p = r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\access\MirageAccessibilityService.kt'
with open(p,'rb') as f: lines = f.read().decode('utf-8','replace').splitlines()
for i in range(59, 200): print(f'{i+1}: {lines[i]}')
