import os

# AccessoryCommandReceiver残り (L70-155)
p = r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\ipc\AccessoryCommandReceiver.kt'
with open(p,'rb') as f: lines = f.read().decode('utf-8','replace').splitlines()
for i in range(69, len(lines)): print(f'{i+1}: {lines[i]}')
print()

# MirageAccessibilityService (L1-120)
p2 = r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\access\MirageAccessibilityService.kt'
with open(p2,'rb') as f: lines2 = f.read().decode('utf-8','replace').splitlines()
print(f'=== MirageAccessibilityService ({len(lines2)}L) - first 130 lines ===')
for i in range(min(130, len(lines2))): print(f'{i+1}: {lines2[i]}')
