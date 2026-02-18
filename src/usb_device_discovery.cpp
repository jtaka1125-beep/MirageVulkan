#include "multi_usb_command_sender.hpp"

#ifdef USE_LIBUSB

#include <chrono>
#include <thread>
#include "mirage_log.hpp"
#include "mirage_protocol.hpp"

using namespace mirage::protocol;

namespace gui {

bool MultiUsbCommandSender::find_and_open_all_devices() {
    libusb_device** devs;
    ssize_t cnt = libusb_get_device_list(ctx_, &devs);
    if (cnt < 0) {
        return false;
    }

    // First pass: find and open existing AOA devices
    uint16_t aoa_pids[] = {
        AOA_PID_ACCESSORY,
        AOA_PID_ACCESSORY_ADB,
        AOA_PID_ACCESSORY_AUDIO,
        AOA_PID_ACCESSORY_AUDIO_ADB
    };

    bool found_any = false;
    std::vector<libusb_device*> android_devices;

    for (ssize_t i = 0; i < cnt; i++) {
        libusb_device* dev = devs[i];
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(dev, &desc) != 0) continue;

        // Check if AOA device
        if (desc.idVendor == AOA_VID) {
            for (uint16_t pid : aoa_pids) {
                if (desc.idProduct == pid) {
                    if (open_aoa_device(dev, pid)) {
                        found_any = true;
                    }
                    break;
                }
            }
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

    // Second pass: switch Android devices to AOA mode
    if (!android_devices.empty()) {
        bool switched = false;
        for (auto dev : android_devices) {
            struct libusb_device_descriptor desc;
            libusb_get_device_descriptor(dev, &desc);
            MLOG_INFO("multicmd", "Found Android device (VID=%04x PID=%04x), switching to AOA", desc.idVendor, desc.idProduct);
            if (switch_device_to_aoa_mode(dev)) {
                switched = true;
            }
        }

        if (switched) {
            libusb_free_device_list(devs, 1);
            devs = nullptr;

            // WinUSBバインド完了待ち + リトライ（最大5回、各1秒間隔）
            constexpr int AOA_OPEN_MAX_RETRIES = 5;
            constexpr int AOA_OPEN_RETRY_INTERVAL_MS = 1000;

            MLOG_INFO("multicmd", "Waiting for devices to re-enumerate...");
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));

            for (int retry = 0; retry < AOA_OPEN_MAX_RETRIES; retry++) {
                cnt = libusb_get_device_list(ctx_, &devs);
                if (cnt < 0) {
                    MLOG_WARN("multicmd", "libusb_get_device_list failed on retry %d/%d",
                              retry + 1, AOA_OPEN_MAX_RETRIES);
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

                    if (desc.idVendor == AOA_VID) {
                        for (uint16_t pid : aoa_pids) {
                            if (desc.idProduct == pid) {
                                found_aoa = true;
                                if (open_aoa_device(dev, pid)) {
                                    found_any = true;
                                } else {
                                    // openに失敗 — まだWinUSBバインド未完了の可能性
                                    all_opened = false;
                                }
                                break;
                            }
                        }
                    }
                }

                libusb_free_device_list(devs, 1);
                devs = nullptr;

                if (found_aoa && all_opened) {
                    // 全AOAデバイスのopen成功
                    break;
                }

                if (retry < AOA_OPEN_MAX_RETRIES - 1) {
                    MLOG_INFO("multicmd", "AOA device open failed, retrying (%d/%d)...",
                              retry + 1, AOA_OPEN_MAX_RETRIES);
                    std::this_thread::sleep_for(std::chrono::milliseconds(AOA_OPEN_RETRY_INTERVAL_MS));
                } else {
                    MLOG_WARN("multicmd", "AOA device open failed after %d retries (WinUSBバインド未完了の可能性)",
                              AOA_OPEN_MAX_RETRIES);
                }
            }
        }
    }

    if (devs) {
        libusb_free_device_list(devs, 1);
    }
    return found_any;
}

} // namespace gui

#endif // USE_LIBUSB
