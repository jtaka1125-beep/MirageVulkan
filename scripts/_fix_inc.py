path = r"C:\MirageWork\MirageVulkan\src\auto_setup.hpp"
with open(path, "r", encoding="utf-8") as f:
    c = f.read()
c = c.replace("#include <functional>", "#include <functional>\n#include <thread>", 1)
with open(path, "w", encoding="utf-8") as f:
    f.write(c)
print("OK")
