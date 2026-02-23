
p=r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\access\MirageAccessibilityService.kt'
with open(p,'rb') as f: c=f.read().decode('utf-8')
lines=c.splitlines()
for i,l in enumerate(lines,1):
    if 'fun pinch' in l or 'fun longPress' in l or 'fun swipe' in l or 'fun tap(' in l or 'fun clickBy' in l:
        print(f'{i}: {l}')
