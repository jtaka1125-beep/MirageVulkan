
p = r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\access\MirageAccessibilityService.kt'
with open(p,'rb') as f: text=f.read().decode('utf-8')
lines = text.splitlines()
print(f"Total: {len(lines)} lines")
for i,l in enumerate(lines,1):
    if 'fun ' in l:
        print(f'{i}: {l}')
