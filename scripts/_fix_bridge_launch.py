path = r"C:\MirageWork\MirageVulkan\src\auto_setup.hpp"
with open(path, 'r', encoding='utf-8') as f:
    c = f.read()

old = '''        // 5. Wait for server to start
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));

        if (progress_callback_)
            progress_callback_("scrcpy server ready, TCP port: " + std::to_string(tcp_port_), 50);

        // No bridge needed - MirrorReceiver connects directly via TCP

        result.status = SetupStatus::COMPLETED;
        result.message = "scrcpy started";
        return result;'''

new = '''        // 5. Wait for server to start, then launch TCP->UDP bridge
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));

        if (progress_callback_)
            progress_callback_("Connecting to scrcpy stream...", 50);

        // Start bridge thread: TCP (scrcpy) -> UDP (MirrorReceiver)
        bridge_running_ = true;
        bridge_thread_ = std::thread(&AutoSetup::bridge_loop, this);

        result.status = SetupStatus::COMPLETED;
        result.message = "scrcpy started";
        return result;'''

c = c.replace(old, new)

with open(path, 'w', encoding='utf-8') as f:
    f.write(c)
print("FIXED: Restored bridge thread launch in start_screen_capture")
