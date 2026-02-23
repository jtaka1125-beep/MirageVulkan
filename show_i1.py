
p = r'C:\MirageWork\MirageVulkan\src\hybrid_command_sender.cpp'
with open(p,'rb') as f: c=f.read().decode('utf-8')
lines=c.splitlines()
for i in range(293, 335):
    print(f'{i+1}: {lines[i]}')
