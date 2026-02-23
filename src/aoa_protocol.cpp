#include "multi_usb_command_sender.hpp"

#ifdef USE_LIBUSB

#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>
#include "mirage_log.hpp"
#include "mirage_protocol.hpp"
#include "winusb_checker.hpp"

using namespace mirage::protocol;

namespace gui {

std::string MultiUsbCommandSender::get_usb_serial(libusb_device_handle* handle, libusb_device_descriptor& desc) {
    if (desc.iSerialNumber == 0) return "";

    char serial[256] = {0};
    int ret = libusb_get_string_descriptor_ascii(handle, desc.iSerialNumber,
                                                  (unsigned char*)serial, sizeof(serial) - 1);
    if (ret > 0) {
        return std::string(serial);
    }
    return "";
}

std::string MultiUsbCommandSender::make_usb_id(libusb_device* dev, const std::string& serial) {
    uint8_t bus = libusb_get_bus_number(dev);
    uint8_t addr = libusb_get_device_address(dev);

    if (!serial.empty()) {
        // Use serial if available
        return serial;
    }

    // Fall back to bus:address
    char id[32];
    snprintf(id, sizeof(id), "%d:%d", bus, addr);
    return std::string(id);
}

int MultiUsbCommandSender::get_aoa_protocol_version(libusb_device_handle* handle) {
    uint8_t version[2] = {0, 0};
    int ret = libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR,
        AOA_GET_PROTOCOL,
        0, 0,
        version, sizeof(version),
        1000
    );
    if (ret < 0) {
        return -1;
    }
    return version[0] | (version[1] << 8);
}

int MultiUsbCommandSender::check_aoa_version(libusb_device* dev) {
    libusb_device_handle* handle = nullptr;
    int ret = libusb_open(dev, &handle);
    if (ret != LIBUSB_SUCCESS) {
        return -1;
    }
    int version = get_aoa_protocol_version(handle);
    libusb_close(handle);
    return version;
}

bool MultiUsbCommandSender::send_aoa_string(libusb_device_handle* handle, uint16_t index, const char* str) {
    int ret = libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
        AOA_SEND_STRING,
        0, index,
        (unsigned char*)str, strlen(str) + 1,
        1000
    );
    return ret >= 0;
}

bool MultiUsbCommandSender::switch_device_to_aoa_mode(libusb_device* dev) {
    libusb_device_handle* handle = nullptr;
    int ret = libusb_open(dev, &handle);
    if (ret != LIBUSB_SUCCESS) {
        if (ret == LIBUSB_ERROR_ACCESS || ret == LIBUSB_ERROR_NOT_SUPPORTED) {
            MLOG_ERROR("multicmd", "Cannot open device for AOA switch: %s (WinUSB driver not installed - ADB fallback will be used)", libusb_error_name(ret));
        } else {
            MLOG_ERROR("multicmd", "Failed to open device for AOA switch: %s", libusb_error_name(ret));
        }
        return false;
    }

    // Check if device supports AOA
    int aoa_version = get_aoa_protocol_version(handle);
    if (aoa_version < 1) {
        MLOG_INFO("multicmd", "Device does not support AOA protocol");
        libusb_close(handle);
        return false;
    }
    MLOG_INFO("multicmd", "Device supports AOA protocol version %d", aoa_version);

    // Send accessory identification strings
    if (!send_aoa_string(handle, AOA_STRING_MANUFACTURER, "Mirage") ||
        !send_aoa_string(handle, AOA_STRING_MODEL, "MirageCtl") ||
        !send_aoa_string(handle, AOA_STRING_DESCRIPTION, "Mirage Control Interface") ||
        !send_aoa_string(handle, AOA_STRING_VERSION, "1") ||
        !send_aoa_string(handle, AOA_STRING_URI, "https://github.com/mirage") ||
        !send_aoa_string(handle, AOA_STRING_SERIAL, "MirageCtl001")) {
        MLOG_ERROR("multicmd", "Failed to send AOA strings");
        libusb_close(handle);
        return false;
    }

    // AOA v2: Register HID devices before starting accessory mode
    if (aoa_version >= 2 && pre_start_callback_) {
        MLOG_INFO("multicmd", "AOA v2 detected, invoking pre-start callback for HID registration");
        if (!pre_start_callback_(handle, aoa_version)) {
            MLOG_WARN("multicmd", "Pre-start callback failed, continuing without HID");
        }
    }

    // Start accessory mode
    ret = libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
        AOA_START_ACCESSORY,
        0, 0,
        nullptr, 0,
        1000
    );
    if (ret < 0) {
        MLOG_ERROR("multicmd", "Failed to start accessory mode: %s", libusb_error_name(ret));
        libusb_close(handle);
        return false;
    }

    MLOG_INFO("multicmd", "Sent AOA start, device will re-enumerate");
    libusb_close(handle);
    return true;
}

bool MultiUsbCommandSender::open_aoa_device(libusb_device* dev, uint16_t pid) {
    libusb_device_descriptor desc;
    if (libusb_get_device_descriptor(dev, &desc) != 0) {
        return false;
    }

    uint8_t bus = libusb_get_bus_number(dev);
    uint8_t addr = libusb_get_device_address(dev);

    libusb_device_handle* handle = nullptr;
    int ret = libusb_open(dev, &handle);

    // LIBUSB_ERROR_ACCESS: OS handle leaked from a previous process that crashed/exited
    // without properly closing. Attempt a USB reset to release the OS handle.
    if (ret == LIBUSB_ERROR_ACCESS) {
        MLOG_WARN("multicmd", "ACCESS DENIED on open (VID=%04x PID=%04x bus=%d addr=%d): "
                  "trying libusb_reset_device to clear leaked OS handle...",
                  desc.idVendor, pid, bus, addr);

        // We can't call reset without a handle, so open with a temporary context trick:
        // re-enumerate by waiting briefly (WinUSB sometimes clears on its own).
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        ret = libusb_open(dev, &handle);

        if (ret == LIBUSB_ERROR_ACCESS) {
            // Still locked — try a USB device reset via a temporary handle if possible
            // (some WinUSB versions allow open even when "claimed")
            MLOG_ERROR("multicmd", "Still ACCESS DENIED after delay. "
                       "Replugging the USB cable will resolve this permanently.");
            return false;
        }
    }

    if (ret != LIBUSB_SUCCESS) {
        MLOG_ERROR("multicmd", "Failed to open AOA device (VID=%04x PID=%04x bus=%d addr=%d): %s",
                   desc.idVendor, pid, bus, addr, libusb_error_name(ret));
        if (ret == LIBUSB_ERROR_NOT_SUPPORTED) {
            MLOG_ERROR("multicmd", "DRIVER ISSUE: WinUSB not installed for this device. "
                       "Use GUI [Driver Setup] or run install_android_winusb.py");
        } else if (ret == LIBUSB_ERROR_IO) {
            MLOG_INFO("multicmd", "IO ERROR: WinUSB not ready yet (caller will retry)");
        } else {
            MLOG_INFO("multicmd", "Hint: Check USB cable and device connection");
        }
        return false;
    }

    // Get serial number
    std::string serial = get_usb_serial(handle, desc);
    std::string usb_id = make_usb_id(dev, serial);

    // Check if already tracked
    {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        if (devices_.find(usb_id) != devices_.end()) {
            libusb_close(handle);
            return false; // Already have this device
        }
    }

    // Try to detach kernel driver if attached (Linux) or set auto-detach
    libusb_set_auto_detach_kernel_driver(handle, 1);

    // Claim interface 0
    ret = libusb_claim_interface(handle, 0);
    if (ret != LIBUSB_SUCCESS) {
        MLOG_ERROR("multicmd", "Failed to claim interface for %s: %s", usb_id.c_str(), libusb_error_name(ret));
        libusb_close(handle);
        return false;
    }

    // Find bulk endpoints
    struct libusb_config_descriptor* config;
    if (libusb_get_active_config_descriptor(dev, &config) != LIBUSB_SUCCESS) {
        MLOG_ERROR("multicmd", "Failed to get config descriptor for %s", usb_id.c_str());
        libusb_release_interface(handle, 0);
        libusb_close(handle);
        return false;
    }

    uint8_t ep_out = 0, ep_in = 0;
    for (int i = 0; i < config->interface[0].altsetting[0].bNumEndpoints; i++) {
        const struct libusb_endpoint_descriptor* ep = &config->interface[0].altsetting[0].endpoint[i];
        if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK) {
            if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT) {
                ep_out = ep->bEndpointAddress;
            } else {
                ep_in = ep->bEndpointAddress;
            }
        }
    }
    libusb_free_config_descriptor(config);

    if (ep_out == 0) {
        MLOG_INFO("multicmd", "No bulk OUT endpoint for %s", usb_id.c_str());
        libusb_release_interface(handle, 0);
        libusb_close(handle);
        return false;
    }

    // Create device handle
    auto device = std::make_unique<DeviceHandle>();
    device->info.usb_id = usb_id;
    device->info.serial = serial;
    device->info.bus = libusb_get_bus_number(dev);
    device->info.address = libusb_get_device_address(dev);
    device->info.connected = true;
    device->handle = handle;
    device->ep_out = ep_out;
    device->ep_in = ep_in;

    MLOG_INFO("multicmd", "Opened AOA device: %s (PID=%04x, ep_out=0x%02x, ep_in=0x%02x)",
              usb_id.c_str(), pid, ep_out, ep_in);

    {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        devices_[usb_id] = std::move(device);
    }

    if (device_opened_callback_) {
        device_opened_callback_(usb_id, handle);
    }

    return true;
}

} // namespace gui

#endif // USE_LIBUSB
