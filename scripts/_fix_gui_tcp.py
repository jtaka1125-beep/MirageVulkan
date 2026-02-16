path = r"C:\MirageWork\MirageVulkan\src\gui\gui_init.cpp"
with open(path, 'r', encoding='utf-8') as f:
    c = f.read()

old = '''        int success = g_adb_manager->startScreenCaptureOnAll(host_ip, 0);  // 0 = use pre-assigned ports
        MLOG_INFO("gui", "Screen capture started: %d/%zu devices", success, device_ids.size());
        return true;'''

new = '''        int success = g_adb_manager->startScreenCaptureOnAll(host_ip, 0);  // 0 = use pre-assigned ports
        MLOG_INFO("gui", "Screen capture started: %d/%zu devices", success, device_ids.size());

        // Switch MirrorReceivers to TCP direct mode (bypass UDP packet loss)
        if (success > 0 && g_multi_receiver) {
            auto devices = g_adb_manager->getUniqueDevices();
            for (const auto& dev : devices) {
                if (dev.assigned_tcp_port > 0) {
                    g_multi_receiver->restart_as_tcp(dev.hardware_id, dev.assigned_tcp_port);
                }
            }
        }
        return true;'''

c = c.replace(old, new)

with open(path, 'w', encoding='utf-8') as f:
    f.write(c)
print("DONE")
