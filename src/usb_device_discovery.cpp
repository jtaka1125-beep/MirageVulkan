#include "multi_usb_command_sender.hpp"

#ifdef USE_LIBUSB

#include <chrono>
#include <thread>
#include "mirage_log.hpp"
#include "mirage_protocol.hpp"

using namespace mirage::protocol;

namespace gui {

bool MultiUsbCommandSender::find_and_open_all_devices(bool allow_wait) {
    libusb_device** devs;
    ssize_t cnt = libusb_get_device_list(ctx_, &devs);
    if (cnt < 0) {
        return false;
    }

    // First pass: collect existing AOA devices and potential Android devices
    uint16_t aoa_pids[] = {
        AOA_PID_ACCESSORY,
        AOA_PID_ACCESSORY_ADB,
        AOA_PID_ACCESSORY_AUDIO,
        AOA_PID_ACCESSORY_AUDIO_ADB
    };
    // Note: Standard Android devices re-enumerate to Google VID (0x18D1) + AOA PIDs after switch.
    // MediaTek exception: VID=0x0E8D/PID=0x201C stays at MTK VID even in AOA mode.
    // PID=0x2005 is RNDIS+ADB, never AOA-switchable via libusb.

    bool found_any = false;
    std::vector<libusb_device*> android_devices;
    std::vector<std::pair<libusb_device*, uint16_t>> aoa_devices_to_open;

    for (ssize_t i = 0; i < cnt; i++) {
        libusb_device* dev = devs[i];
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(dev, &desc) != 0) continue;

        // Check if already-enumerated AOA device (Google VID_18D1 ONLY)
        // After AOA switch, device re-enumerates with Google VID regardless of original vendor
        if (desc.idVendor == AOA_VID) {
            for (uint16_t pid : aoa_pids) {
                if (desc.idProduct == pid) {
                    aoa_devices_to_open.push_back({dev, pid});
                    break;
                }
            }
            continue;
        }

        // MTK (VID=0x0E8D) does NOT re-enumerate to Google VID after AOA switch.
        // PID=0x201C = AOA+ADB composite - treat as already-switched AOA device.
        // Do NOT add to android_devices or switch will be sent again on every rescan.
        if (desc.idVendor == 0x0E8D && desc.idProduct == 0x201C) {
            aoa_devices_to_open.push_back({dev, desc.idProduct});
            continue;
        }

        // Check if potential Android device for AOA switch
        bool is_android = false;
        switch (desc.idVendor) {
            case AOA_VID: // Google
            case 0x04E8: // Samsung
            case 0x22B8: // Motorola
            case 0x0BB4: // HTC
            case 0x12D1: // Huawei
            case 0x2717: // Xiaomi
            case 0x19D2: // ZTE
            case 0x1004: // LG
            case 0x0FCE: // Sony
            case 0x2A70: // OnePlus
            case 0x0E8D: // MediaTek
            case 0x1782: // Spreadtrum
            case 0x1F3A: // Allwinner
            case 0x2207: // Rockchip
                is_android = true;
                break;
        }

        if (is_android) {
            android_devices.push_back(dev);
        }
    }

    constexpr int AOA_DIRECT_OPEN_RETRIES_WAIT = 8;
    constexpr int AOA_DIRECT_OPEN_RETRIES_NOWAIT = 0;
    constexpr int AOA_DIRECT_OPEN_DELAY_MS = 2000;
    constexpr int AOA_DIRECT_OPEN_INITIAL_MS = 3000;

    int direct_retries = allow_wait ? AOA_DIRECT_OPEN_RETRIES_WAIT : AOA_DIRECT_OPEN_RETRIES_NOWAIT;

    if (!aoa_devices_to_open.empty() && allow_wait) {
        MLOG_INFO("multicmd", "Found %zu AOA device(s), waiting for WinUSB binding...",
                  aoa_devices_to_open.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(AOA_DIRECT_OPEN_INITIAL_MS));
    }

    for (auto& [aoa_dev, pid] : aoa_devices_to_open) {
        bool opened = false;
        for (int retry = 0; retry <= direct_retries; retry++) {
            if (retry > 0) {
                MLOG_INFO("multicmd", "AOA open retry %d/%d (WinUSB may not be ready)...",
                          retry, direct_retries);
                std::this_thread::sleep_for(std::chrono::milliseconds(AOA_DIRECT_OPEN_DELAY_MS));
            }
            if (open_aoa_device(aoa_dev, pid)) {
                found_any = true;
                opened = true;
                break;
            }
        }
        if (!opened) {
            if (allow_wait) {
                MLOG_WARN("multicmd", "Failed to open AOA device after %d retries. "
                          "Try replugging the USB cable.", direct_retries);
            } else {
                MLOG_INFO("multicmd", "AOA device open deferred (WinUSB binding in progress, rescan will retry)");
            }
        }
    }

    // Second pass: switch Android devices to AOA mode
    if (!android_devices.empty()) {
        if (!aoa_auto_switch_) {
            MLOG_INFO("multicmd", "AOA auto-switch disabled by config, skipping %zu Android device(s)",
                      android_devices.size());
        } else {
        bool switched = false;
        for (auto dev : android_devices) {
            struct libusb_device_descriptor desc;
            libusb_get_device_descriptor(dev, &desc);
            // Skip MTK RNDIS+ADB (PID=0x2005): not AOA-switchable via libusb,
            // requires RNDIS driver (not WinUSB). Harmless to skip - it's not AOA.
            if (desc.idVendor == 0x0E8D && desc.idProduct == 0x2005) {
                MLOG_DEBUG("multicmd", "Skipping MTK RNDIS+ADB (PID=0x2005), not AOA-switchable");
                continue;
            }
            MLOG_INFO("multicmd", "Found Android device (VID=%04x PID=%04x), switching to AOA", desc.idVendor, desc.idProduct);
            if (switch_device_to_aoa_mode(dev)) {
                switched = true;
            }
        }

        if (switched) {
            libusb_free_device_list(devs, 1);
            devs = nullptr;

            if (!allow_wait) {
                MLOG_INFO("multicmd", "AOA switch sent, deferred open to rescan thread");
                return false;
            }

            constexpr int AOA_OPEN_MAX_RETRIES = 8;
            constexpr int AOA_OPEN_RETRY_INTERVAL_MS = 2000;

            MLOG_INFO("multicmd", "Waiting for devices to re-enumerate...");
            std::this_thread::sleep_for(std::chrono::milliseconds(3000));

            for (int retry = 0; retry < AOA_OPEN_MAX_RETRIES; retry++) {
                cnt = libusb_get_device_list(ctx_, &devs);
                if (cnt < 0) {
                    devs = nullptr;
                    std::this_thread::sleep_for(std::chrono::milliseconds(AOA_OPEN_RETRY_INTERVAL_MS));
                    continue;
                }

                bool all_opened = true;
                bool found_aoa = false;

                for (ssize_t i = 0; i < cnt; i++) {
                    libusb_device* dev = devs[i];
                    struct libusb_device_descriptor desc;
                    if (libusb_get_device_descriptor(dev, &desc) != 0) continue;

                    // Check Google VID AOA devices (standard)
                    if (desc.idVendor == AOA_VID) {
                        for (uint16_t pid : aoa_pids) {
                            if (desc.idProduct == pid) {
                                found_aoa = true;
                                if (open_aoa_device(dev, pid)) {
                                    found_any = true;
                                } else {
                                    all_opened = false;
                                }
                                break;
                            }
                        }
                    }
                    // MTK (VID=0x0E8D) stays at its own VID after AOA switch.
                    // PID=0x201C = AOA+ADB composite. Open directly without re-switch.
                    else if (desc.idVendor == 0x0E8D && desc.idProduct == 0x201C) {
                        found_aoa = true;
                        if (open_aoa_device(dev, desc.idProduct)) {
                            found_any = true;
                        } else {
                            all_opened = false;
                        }
                    }
                }

                libusb_free_device_list(devs, 1);
                devs = nullptr;

                if (found_aoa && all_opened) break;

                if (retry < AOA_OPEN_MAX_RETRIES - 1) {
                    MLOG_INFO("multicmd", "AOA device open failed, retrying (%d/%d)...",
                              retry + 1, AOA_OPEN_MAX_RETRIES);
                    std::this_thread::sleep_for(std::chrono::milliseconds(AOA_OPEN_RETRY_INTERVAL_MS));
                } else {
                    MLOG_WARN("multicmd", "AOA device open failed after %d retries",
                              AOA_OPEN_MAX_RETRIES);
                }
            }
        }
        } // else: aoa_auto_switch_ enabled
    }

    if (devs) {
        libusb_free_device_list(devs, 1);
    }
    return found_any;
}

} // namespace gui

#endif // USE_LIBUSB
