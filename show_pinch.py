
p = r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\access\MirageAccessibilityService.kt'
with open(p,'rb') as f: text=f.read().decode('utf-8')
lines = text.splitlines()
for i in range(150, 215):
    print(f'{i+1}: {lines[i]}')
