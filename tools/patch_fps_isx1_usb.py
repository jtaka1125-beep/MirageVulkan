from pathlib import Path
import re
p=Path(r"C:/MirageWork/MirageVulkan/src/gui/gui_init.cpp")
t=p.read_text(encoding='utf-8', errors='ignore')
# find isX1 definition in FPS callback
pat=re.compile(r"const bool isX1 = \(dev\.display_name\.find\(\"Npad X1\"\) != std::string::npos\) \|\|\s*\n\s*\(dev\.display_name\.find\(\"N-one Npad X1\"\) != std::string::npos\) \|\|\s*\n\s*\(dev\.preferred_adb_id\.find\(\"192\.168\.0\.3:5555\"\) != std::string::npos\);", re.MULTILINE)
m=pat.search(t)
if not m:
    raise SystemExit('isX1 block not found')
new=(
    'const bool isX1 = (dev.display_name.find("Npad X1") != std::string::npos) ||\n'
    '                                       (dev.display_name.find("N-one Npad X1") != std::string::npos) ||\n'
    '                                       (dev.preferred_adb_id.find("192.168.0.3:5555") != std::string::npos) ||\n'
    '                                       (dev.preferred_adb_id.find("93020523431940") != std::string::npos);'
)
t=t[:m.start()]+new+t[m.end():]
p.write_text(t, encoding='utf-8')
print('patched isX1 to include USB serial')
