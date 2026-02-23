
p = r'C:\MirageWork\MirageVulkan\src\multi_usb_command_sender.cpp'
with open(p,'rb') as f: text=f.read().decode('utf-8')
lines = text.splitlines()
for i in range(500, 560):
    print(f'{i+1}: {lines[i]}')
