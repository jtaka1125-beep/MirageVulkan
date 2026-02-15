// aoa_reset.cpp - Reset AOA devices back to normal USB mode
#include <cstdio>
#include <libusb-1.0/libusb.h>

int main() {
    libusb_context* ctx;
    libusb_init(&ctx);
    
    libusb_device** devs;
    ssize_t cnt = libusb_get_device_list(ctx, &devs);
    int reset_count = 0;
    
    for (ssize_t i = 0; i < cnt; i++) {
        libusb_device_descriptor desc;
        libusb_get_device_descriptor(devs[i], &desc);
        if (desc.idVendor != 0x18D1) continue;
        if (desc.idProduct < 0x2D00 || desc.idProduct > 0x2D05) continue;
        
        printf("AOA device: PID=0x%04X bus=%d addr=%d\n",
               desc.idProduct,
               libusb_get_bus_number(devs[i]),
               libusb_get_device_address(devs[i]));
        
        libusb_device_handle* h;
        if (libusb_open(devs[i], &h) == 0) {
            int r = libusb_reset_device(h);
            printf("  Reset: %s\n", r == 0 ? "OK" : libusb_error_name(r));
            libusb_close(h);
            reset_count++;
        } else {
            printf("  Open failed\n");
        }
    }
    
    libusb_free_device_list(devs, 1);
    libusb_exit(ctx);
    printf("\nReset %d device(s)\n", reset_count);
    return 0;
}
