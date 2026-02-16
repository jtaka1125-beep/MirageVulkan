path = r"C:\MirageWork\MirageVulkan\src\mirror_receiver.hpp"
with open(path, "r", encoding="utf-8") as f:
    content = f.read()

# Replace getPort with a version that waits for bind completion
old = '  uint16_t getPort() const { return bound_port_.load(); }'
new = '''  uint16_t getPort(int timeout_ms = 2000) const {
    auto start = std::chrono::steady_clock::now();
    while (bound_port_.load() == 0) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start).count();
      if (elapsed > timeout_ms) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return bound_port_.load();
  }'''

if old in content:
    content = content.replace(old, new, 1)
    # Add chrono include if not present
    if '#include <chrono>' not in content:
        content = content.replace('#include <atomic>', '#include <atomic>\n#include <chrono>\n#include <thread>', 1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
    print("PATCHED: getPort() now waits for bind")
else:
    print("NOT FOUND")
