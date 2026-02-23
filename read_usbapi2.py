p = r'C:\MirageWork\MirageVulkan\src\usb_command_api.cpp'
with open(p,'rb') as f: lines = f.read().decode('utf-8','replace').splitlines()
for i in range(80, len(lines)): print(f'{i+1}: {lines[i]}')
