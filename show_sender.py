
p = r'C:\MirageWork\MirageVulkan\src\multi_usb_command_sender.cpp'
with open(p,'rb') as f: text=f.read().decode('utf-8')
lines = text.splitlines()
print(f'total: {len(lines)}')
for i,l in enumerate(lines,1):
    if any(k in l for k in ['ack_callback_','queue_command','send_raw']):
        print(f'{i}: {l}')
