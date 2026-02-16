path = r"C:\MirageWork\MirageVulkan\src\gui\gui_init.cpp"
with open(path, "r", encoding="utf-8") as f:
    content = f.read()

old = '''        auto devices = g_adb_manager->getUniqueDevices();
        for (const auto& dev : devices) {
            MLOG_INFO("gui", "  - %s -> port %d", dev.display_name.c_str(), dev.assigned_port);
        }'''

new = '''        // Port info is logged by assignPorts() in AdbDeviceManager'''

if old in content:
    content = content.replace(old, new, 1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
    print("PATCHED: removed redundant port log")
else:
    print("NOT FOUND")
