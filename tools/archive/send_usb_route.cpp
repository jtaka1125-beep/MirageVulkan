// Simple tool to send VIDEO_ROUTE command to switch Android to USB mode
// Compile: g++ -o send_usb_route.exe send_usb_route.cpp -lusb-1.0

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <libusb-1.0/libusb.h>

// Protocol constants
static constexpr uint8_t CMD_VIDEO_ROUTE = 0x25;
static constexpr uint32_t MIRA_MAGIC = 0x4D495241;  // "MIRA"
static constexpr uint8_t PROTO_VERSION = 1;

// Build MIRA packet
int build_packet(uint8_t* buf, uint8_t cmd, uint32_t seq, const uint8_t* payload, size_t payload_len) {
    // Header: MIRA(4) + ver(1) + cmd(1) + seq(4) + len(4) = 14 bytes
    buf[0] = 'M'; buf[1] = 'I'; buf[2] = 'R'; buf[3] = 'A';
    buf[4] = PROTO_VERSION;
    buf[5] = cmd;
    // seq (big-endian)
    buf[6] = (seq >> 24) & 0xFF;
    buf[7] = (seq >> 16) & 0xFF;
    buf[8] = (seq >> 8) & 0xFF;
    buf[9] = seq & 0xFF;
    // len (big-endian)
    uint32_t len = payload_len;
    buf[10] = (len >> 24) & 0xFF;
    buf[11] = (len >> 16) & 0xFF;
    buf[12] = (len >> 8) & 0xFF;
    buf[13] = len & 0xFF;
    // payload
    if (payload && payload_len > 0) {
        memcpy(buf + 14, payload, payload_len);
    }
    return 14 + payload_len;
}

int main(int argc, char** argv) {
    uint8_t mode = 0;  // 0=USB, 1=WiFi
    const char* host = "192.168.0.8";
    int port = 60000;

    if (argc > 1) mode = atoi(argv[1]);
    if (argc > 2) host = argv[2];
    if (argc > 3) port = atoi(argv[3]);

    printf("Sending VIDEO_ROUTE: mode=%d (%s) host=%s port=%d\n",
           mode, mode ? "WiFi" : "USB", host, port);

    // Init libusb
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0) {
        fprintf(stderr, "Failed to init libusb\n");
        return 1;
    }

    // Find AOA device (VID=0x18D1, PID=0x2D01)
    libusb_device** devs;
    ssize_t cnt = libusb_get_device_list(ctx, &devs);
    if (cnt < 0) {
        fprintf(stderr, "Failed to get device list\n");
        libusb_exit(ctx);
        return 1;
    }

    libusb_device_handle* handle = nullptr;
    for (ssize_t i = 0; i < cnt; i++) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(devs[i], &desc) != 0) continue;

        if (desc.idVendor == 0x18D1 && desc.idProduct == 0x2D01) {
            if (libusb_open(devs[i], &handle) == 0) {
                printf("Found AOA device\n");
                break;
            }
        }
    }
    libusb_free_device_list(devs, 1);

    if (!handle) {
        fprintf(stderr, "No AOA device found\n");
        libusb_exit(ctx);
        return 1;
    }

    // Claim interface
    libusb_set_auto_detach_kernel_driver(handle, 1);
    if (libusb_claim_interface(handle, 0) != 0) {
        fprintf(stderr, "Failed to claim interface\n");
        libusb_close(handle);
        libusb_exit(ctx);
        return 1;
    }

    // Build VIDEO_ROUTE payload: mode(1) + host_len(1) + host + port(2)
    uint8_t payload[256];
    size_t host_len = strlen(host);
    payload[0] = mode;
    payload[1] = (uint8_t)host_len;
    memcpy(payload + 2, host, host_len);
    payload[2 + host_len] = (port >> 8) & 0xFF;
    payload[3 + host_len] = port & 0xFF;
    size_t payload_len = 4 + host_len;

    // Build packet
    uint8_t packet[512];
    int packet_len = build_packet(packet, CMD_VIDEO_ROUTE, 1, payload, payload_len);

    // Send
    int transferred = 0;
    int ret = libusb_bulk_transfer(handle, 0x01, packet, packet_len, &transferred, 1000);
    if (ret == 0) {
        printf("Sent %d bytes\n", transferred);
    } else {
        fprintf(stderr, "Send failed: %s\n", libusb_error_name(ret));
    }

    libusb_release_interface(handle, 0);
    libusb_close(handle);
    libusb_exit(ctx);
    return ret == 0 ? 0 : 1;
}
