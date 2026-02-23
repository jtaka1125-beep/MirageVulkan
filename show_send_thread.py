
p = r'C:\MirageWork\MirageVulkan\src\multi_usb_command_sender.cpp'
with open(p,'rb') as f: text=f.read().decode('utf-8')
lines = text.splitlines()
# show send_thread area (around line 320-360)
for i in range(315, 380):
    print(f'{i+1}: {lines[i]}')
