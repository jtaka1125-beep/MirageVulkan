void AdbDeviceManager::assignPorts(int base_port) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Port ranges:
    //   video:   base_port + offset     (default: 60000, 60001, 60002, ...)
    //   command: 50000 + offset         (50000, 50001, 50002, ...)
    //   ADB:    5555 + offset           (5555, 5556, 5557, ...)
    if (base_port < 1024 || base_port > 65500) {
        fprintf(stderr, "[ADB] Invalid base port %d, using default 60000\n", base_port);
        base_port = 60000;
    }

    int port_offset = 0;
    for (auto& [hw_id, device] : unique_devices_) {
        device.assigned_port = base_port + port_offset;          // video
        device.assigned_command_port = 50000 + port_offset;      // command
        device.assigned_adb_port = 5555 + port_offset;           // Wi-Fi ADB

        if (device.assigned_port > 65535 || device.assigned_command_port > 65535) {
            fprintf(stderr, "[ADB] Port overflow for %s\n", device.display_name.c_str());
            device.assigned_port = 0;
            device.assigned_command_port = 0;
            device.assigned_adb_port = 0;
            continue;
        }

        fprintf(stderr, "[ADB] Ports for %s: video=%d cmd=%d adb=%d\n",
                device.display_name.c_str(),
                device.assigned_port,
                device.assigned_command_port,
                device.assigned_adb_port);
        port_offset++;
    }
}

int AdbDeviceManager::enableWifiAdbOnAll() {
    // Must be called WITHOUT mutex_ held (calls adbCommand which may lock)
    // Get device info snapshot first
    std::vector<std::pair<std::string, std::string>> usb_devices;  // hw_id, usb_adb_id
    std::vector<std::pair<std::string, int>> adb_ports;            // hw_id, port
    std::vector<std::pair<std::string, std::string>> ip_addresses; // hw_id, ip

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [hw_id, device] : unique_devices_) {
            if (device.usb_connections.empty()) continue;
            if (device.assigned_adb_port == 0) continue;
            usb_devices.push_back({hw_id, device.usb_connections[0]});
            adb_ports.push_back({hw_id, device.assigned_adb_port});
            ip_addresses.push_back({hw_id, device.ip_address});
        }
    }

    int success_count = 0;
    for (size_t i = 0; i < usb_devices.size(); i++) {
        const auto& [hw_id, usb_id] = usb_devices[i];
        int port = adb_ports[i].second;
        const auto& ip = ip_addresses[i].second;

        // Enable tcpip mode on unique port
        std::string tcpip_result = adbCommand(usb_id, "tcpip " + std::to_string(port));

        if (tcpip_result.find("restarting") != std::string::npos) {
            fprintf(stderr, "[ADB] tcpip %d enabled on %s\n", port, usb_id.c_str());

            // Wait for device to restart
            std::this_thread::sleep_for(std::chrono::seconds(2));

            // Connect via Wi-Fi
            if (!ip.empty()) {
                std::string connect_id = ip + ":" + std::to_string(port);
                std::string connect_result = adbCommand(connect_id, "");
                // Use raw adb connect
                std::string cmd = "adb connect " + connect_id + " 2>&1";
                std::array<char, 4096> buffer;
                std::string result;
                UniquePipe pipe(popen(cmd.c_str(), "r"));
                if (pipe) {
                    while (fgets(buffer.data(), buffer.size(), pipe.get()))
                        result += buffer.data();
                }

                if (result.find("connected") != std::string::npos) {
                    fprintf(stderr, "[ADB] Wi-Fi ADB connected: %s\n", connect_id.c_str());
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto it = unique_devices_.find(hw_id);
                    if (it != unique_devices_.end()) {
                        it->second.wifi_connections.push_back(connect_id);
                    }
                    success_count++;
                } else {
                    fprintf(stderr, "[ADB] Wi-Fi ADB connect failed: %s\n", result.c_str());
                }
            }
        } else {
            fprintf(stderr, "[ADB] tcpip failed on %s: %s\n", usb_id.c_str(), tcpip_result.c_str());
        }
    }

    fprintf(stderr, "[ADB] Wi-Fi ADB enabled on %d/%zu devices\n", success_count, usb_devices.size());
    return success_count;
}

