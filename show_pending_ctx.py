
p = r'C:\MirageWork\MirageVulkan\src\hybrid_command_sender.cpp'
with open(p,'rb') as f: c=f.read().decode('utf-8').replace('\r\n','\n')

# Show context around line 37 and 43
lines = c.splitlines()
for i in range(30, 60):
    print(f'{i+1}: {lines[i]}')
