// AOA Switch Test - standalone libusb test
// Build: g++ -o aoa_test.exe aoa_test.cpp -lusb-1.0
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>
#include <libusb-1.0/libusb.h>

#define AOA_GET_PROTOCOL    51
#define AOA_SEND_STRING     52
#define AOA_START_ACCESSORY 53

bool send_aoa_string(libusb_device_handle* h, uint16_t idx, const char* s) {
    int r = libusb_control_transfer(h,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
        AOA_SEND_STRING, 0, idx,
        (unsigned char*)s, strlen(s)+1, 1000);
    return r >= 0;
}

int main() {
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0) {
        fprintf(stderr, "libusb init failed\n");
        return 1;
    }

    libusb_device** devs;
    ssize_t cnt = libusb_get_device_list(ctx, &devs);
    printf("Found %d USB devices\n", (int)cnt);

    int switched = 0;

    for (ssize_t i = 0; i < cnt; i++) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(devs[i], &desc) != 0) continue;

        // MediaTek or other Android VIDs
        if (desc.idVendor != 0x0E8D && desc.idVendor != 0x18D1) continue;

        // Skip if already AOA
        if (desc.idVendor == 0x18D1 && desc.idProduct >= 0x2D00 && desc.idProduct <= 0x2D05) {
            printf("  [%zd] Already AOA: VID=%04x PID=%04x\n", i, desc.idVendor, desc.idProduct);
            continue;
        }

        printf("  [%zd] Android device: VID=%04x PID=%04x\n", i, desc.idVendor, desc.idProduct);

        libusb_device_handle* h = nullptr;
        int r = libusb_open(devs[i], &h);
        if (r != 0) {
            printf("    -> OPEN FAILED: %s\n", libusb_error_name(r));
            continue;
        }
        printf("    -> Opened OK!\n");

        // Get AOA protocol version
        uint8_t ver[2] = {0};
        r = libusb_control_transfer(h,
            LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR,
            AOA_GET_PROTOCOL, 0, 0, ver, 2, 1000);
        if (r < 0) {
            printf("    -> AOA protocol query failed: %s\n", libusb_error_name(r));
            libusb_close(h);
            continue;
        }
        int aoa_ver = ver[0] | (ver[1] << 8);
        printf("    -> AOA protocol version: %d\n", aoa_ver);

        // Send strings
        if (!send_aoa_string(h, 0, "Mirage") ||
            !send_aoa_string(h, 1, "MirageCtl") ||
            !send_aoa_string(h, 2, "Mirage Control") ||
            !send_aoa_string(h, 3, "1") ||
            !send_aoa_string(h, 4, "https://github.com/mirage") ||
            !send_aoa_string(h, 5, "MirageCtl001")) {
            printf("    -> Failed to send AOA strings\n");
            libusb_close(h);
            continue;
        }

        // Start accessory mode
        r = libusb_control_transfer(h,
            LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
            AOA_START_ACCESSORY, 0, 0, nullptr, 0, 1000);
        if (r < 0) {
            printf("    -> AOA START failed: %s\n", libusb_error_name(r));
        } else {
            printf("    -> AOA START sent! Device will re-enumerate.\n");
            switched++;
        }
        libusb_close(h);
    }

    libusb_free_device_list(devs, 1);

    if (switched > 0) {
        printf("\nSwitched %d device(s). Waiting 3s for re-enumeration...\n", switched);
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));

        // Re-scan
        cnt = libusb_get_device_list(ctx, &devs);
        printf("After re-enum: %d USB devices\n", (int)cnt);
        int aoa_found = 0;
        for (ssize_t i = 0; i < cnt; i++) {
            libusb_device_descriptor desc;
            if (libusb_get_device_descriptor(devs[i], &desc) != 0) continue;
            if (desc.idVendor == 0x18D1 && desc.idProduct >= 0x2D00 && desc.idProduct <= 0x2D05) {
                printf("  AOA device found: VID=%04x PID=%04x\n", desc.idVendor, desc.idProduct);
                aoa_found++;
            }
        }
        printf("Total AOA devices: %d\n", aoa_found);
        libusb_free_device_list(devs, 1);
    } else {
        printf("\nNo devices switched.\n");
    }

    libusb_exit(ctx);
    return 0;
}
