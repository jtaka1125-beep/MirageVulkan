
p = r'C:\MirageWork\MirageVulkan\src\usb_command_api.cpp'
with open(p,'rb') as f: raw=f.read()
c = raw.decode('utf-8')
idx = c.find('send_click_id')
print(repr(c[idx:idx+300]))
print('---')
print(repr(c[idx+300:idx+600]))
