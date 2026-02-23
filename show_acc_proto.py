
p = r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\usb\Protocol.kt'
with open(p,'rb') as f: raw=f.read()
c = raw.decode('utf-8')
lines = c.splitlines()

# Show lines 95-195 for full context
for i in range(95, 195):
    print(f'{i+1}: {lines[i]}')
