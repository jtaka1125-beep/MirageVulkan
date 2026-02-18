#!/usr/bin/env python3
"""Patch gui_threads.cpp: Add deferred TCP receiver startup after device discovery"""

filepath = r'C:\MirageWork\MirageVulkan\src\gui\gui_threads.cpp'

with open(filepath, 'r', encoding='utf-8-sig') as f:
    content = f.read()

# Find the early_registration_done = true block and add TCP receiver startup after it
old_block = '''            early_registration_done = true;
        }

        auto now = std::chrono::steady_clock::now();'''

new_block = '''            early_registration_done = true;

            // Deferred TCP receiver startup: now that ADB devices are detected,
            // start TCP video receiver if not already running
            if (!g_tcp_video_receiver && g_adb_manager) {
                auto detected = g_adb_manager->getUniqueDevices();
                if (!detected.empty()) {
                    MLOG_INFO("threads", "Deferred TCP receiver init: %zu devices detected", detected.size());
                    g_tcp_video_receiver = std::make_unique<::gui::TcpVideoReceiver>();
                    g_tcp_video_receiver->setDeviceManager(g_adb_manager.get());
                    if (g_tcp_video_receiver->start()) {
                        auto tcp_ids = g_tcp_video_receiver->getDeviceIds();
                        MLOG_INFO("threads", "TCP video receiver started: %zu device(s)", tcp_ids.size());
                        gui->logInfo(u8"TCP映像レシーバー起動: " + std::to_string(tcp_ids.size()) + u8"台");
                    } else {
                        MLOG_WARN("threads", "TCP video receiver failed to start (will retry via scrcpy)");
                        g_tcp_video_receiver.reset();
                    }
                }
            }
        }

        auto now = std::chrono::steady_clock::now();'''

assert old_block in content, "Target block not found in gui_threads.cpp!"
content = content.replace(old_block, new_block)

with open(filepath, 'w', encoding='utf-8-sig') as f:
    f.write(content)

print("Patch applied: deferred TCP receiver startup after device discovery")
