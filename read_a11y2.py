import os

# MirageAccessibilityService L1-100 (onCreate, onAccessibilityEvent) + L130-311 (gestures)
p = r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\access\MirageAccessibilityService.kt'
with open(p,'rb') as f: lines = f.read().decode('utf-8','replace').splitlines()
print(f'Total: {len(lines)} lines')
print('=== L1-100 ===')
for i in range(min(100, len(lines))): print(f'{i+1}: {lines[i]}')
print('=== L130-180 (gesture middle) ===')
for i in range(129, min(180, len(lines))): print(f'{i+1}: {lines[i]}')
print('=== L200-311 (swipe/pinch/longpress/clickById) ===')
for i in range(199, len(lines)): print(f'{i+1}: {lines[i]}')
