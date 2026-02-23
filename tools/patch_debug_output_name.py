from pathlib import Path
import re
p = Path(r"C:/MirageWork/MirageVulkan/CMakeLists.txt")
t = p.read_text(encoding='utf-8', errors='ignore')
if 'OUTPUT_NAME mirage_vulkan_debug_dev' in t:
    print('already patched');
    raise SystemExit(0)
# Insert after add_executable(mirage_vulkan_debug ...)
pat = r"add_executable\(mirage_vulkan_debug[^\n]*\)\n"
m = re.search(pat, t)
if not m:
    raise SystemExit('pattern not found')
ins = m.group(0) + "# Avoid file lock by scheduled task: write a different debug exe name\n" \
      "set_target_properties(mirage_vulkan_debug PROPERTIES OUTPUT_NAME mirage_vulkan_debug_dev)\n\n"
t = t[:m.start()] + ins + t[m.end():]
p.write_text(t, encoding='utf-8')
print('patched CMakeLists OUTPUT_NAME')
