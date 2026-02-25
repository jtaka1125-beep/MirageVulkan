import pathlib
p=pathlib.Path(r'C:\MirageWork\MirageVulkan\src\config_loader.hpp')
lines=p.read_text(encoding='utf-8',errors='replace').splitlines()
out=[]
seen=0
for l in lines:
    if l.strip()=='const std::map<std::string, ExpectedDeviceSpec>& allDevices() const { return devices_; }':
        seen += 1
        if seen > 1:
            continue
    out.append(l)
p.write_text('\n'.join(out)+'\n',encoding='utf-8')
print('deduped')
