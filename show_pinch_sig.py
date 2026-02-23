
p=r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\access\MirageAccessibilityService.kt'
with open(p,'rb') as f: c=f.read().decode('utf-8')
lines=c.splitlines()
for i in range(152, 165):
    print(f'{i+1}: {lines[i]}')
