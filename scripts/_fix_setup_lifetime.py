import re

# Fix adb_device_manager.hpp - add AutoSetup include and member
hpp_path = r"C:\MirageWork\MirageVulkan\src\adb_device_manager.hpp"
with open(hpp_path, 'r', encoding='utf-8') as f:
    hpp = f.read()

# Add include for auto_setup.hpp and memory
if '#include "auto_setup.hpp"' not in hpp:
    hpp = hpp.replace('#include <cstdint>', '#include <cstdint>\n#include <memory>\n#include "auto_setup.hpp"')

# Add member variable for persistent AutoSetup instances
if 'active_setups_' not in hpp:
    hpp = hpp.replace(
        '    std::map<std::string, UniqueDevice> unique_devices_;  // hardware_id -> UniqueDevice',
        '    std::map<std::string, UniqueDevice> unique_devices_;  // hardware_id -> UniqueDevice\n'
        '    std::map<std::string, std::shared_ptr<mirage::AutoSetup>> active_setups_;  // adb_id -> persistent AutoSetup'
    )

with open(hpp_path, 'w', encoding='utf-8') as f:
    f.write(hpp)

# Fix adb_device_manager.cpp - use shared_ptr instead of stack variable
cpp_path = r"C:\MirageWork\MirageVulkan\src\adb_device_manager.cpp"
with open(cpp_path, 'r', encoding='utf-8') as f:
    cpp = f.read()

# Replace stack AutoSetup with persistent shared_ptr
old_code = '''    // Create AutoSetup with device-specific ADB executor
    mirage::AutoSetup setup;
    setup.set_adb_executor([this, &adb_id](const std::string& cmd) -> std::string {
        // adbCommand expects command like "shell dumpsys...", not just "dumpsys..."
        return adbCommand(adb_id, cmd);
    });

    // Start screen capture
    auto result1 = setup.start_screen_capture(host, port);'''

new_code = '''    // Create persistent AutoSetup (must outlive this function for bridge thread)
    auto setup_ptr = std::make_shared<mirage::AutoSetup>();
    setup_ptr->set_adb_executor([this, adb_id](const std::string& cmd) -> std::string {
        return adbCommand(adb_id, cmd);
    });
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_setups_[adb_id] = setup_ptr;  // Keep alive
    }
    auto& setup = *setup_ptr;

    // Start screen capture
    auto result1 = setup.start_screen_capture(host, port);'''

cpp = cpp.replace(old_code, new_code)

# Also fix the references: setup.approve -> setup.approve (same since setup is now a ref)
# No changes needed there

with open(cpp_path, 'w', encoding='utf-8') as f:
    f.write(cpp)

print("PATCHED hpp and cpp")
