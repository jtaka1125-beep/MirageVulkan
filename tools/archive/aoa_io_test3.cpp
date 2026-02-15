// AOA I/O Test v3 - Try all AOA devices, not just first
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>
#include <libusb-1.0/libusb.h>

#define AOA_GET_PROTOCOL    51
#define AOA_SEND_STRING     52
#define AOA_START_ACCESSORY 53
#define MIRA_MAGIC    0x4D495241
#define MIRA_VERSION  1
#define HEADER_SIZE   14
#define CMD_PING      0
#define CMD_ACK       0x80

#pragma pack(push, 1)
struct MiraHeader {
    uint32_t magic;
    uint8_t  version;
    uint8_t  cmd;
    uint32_t seq;
    uint32_t payload_len;
};
#pragma pack(pop)

static bool send_aoa_string(libusb_device_handle* h, uint16_t idx, const char* s) {
    return libusb_control_transfer(h, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
        AOA_SEND_STRING, 0, idx, (unsigned char*)s, strlen(s)+1, 1000) >= 0;
}

static int switch_to_aoa(libusb_context* ctx) {
    libusb_device** devs;
    ssize_t cnt = libusb_get_device_list(ctx, &devs);
    int switched = 0;
    for (ssize_t i = 0; i < cnt; i++) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(devs[i], &desc) != 0) continue;
        if (desc.idVendor != 0x0E8D) continue;
        libusb_device_handle* h = nullptr;
        if (libusb_open(devs[i], &h) != 0) continue;
        uint8_t ver[2] = {0};
        if (libusb_control_transfer(h, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR,
            AOA_GET_PROTOCOL, 0, 0, ver, 2, 1000) < 0) { libusb_close(h); continue; }
        send_aoa_string(h, 0, "Mirage");
        send_aoa_string(h, 1, "MirageCtl");
        send_aoa_string(h, 2, "Mirage Control");
        send_aoa_string(h, 3, "1");
        send_aoa_string(h, 4, "https://github.com/mirage");
        send_aoa_string(h, 5, "MirageCtl001");
        if (libusb_control_transfer(h, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
            AOA_START_ACCESSORY, 0, 0, nullptr, 0, 1000) >= 0) switched++;
        libusb_close(h);
    }
    libusb_free_device_list(devs, 1);
    return switched;
}

static bool test_ping(libusb_device_handle* h, uint8_t ep_out, uint8_t ep_in, int seq) {
    MiraHeader hdr;
    hdr.magic = MIRA_MAGIC;
    hdr.version = MIRA_VERSION;
    hdr.cmd = CMD_PING;
    hdr.seq = seq;
    hdr.payload_len = 0;

    int transferred = 0;
    int r = libusb_bulk_transfer(h, ep_out, (unsigned char*)&hdr, HEADER_SIZE, &transferred, 2000);
    if (r != 0) { printf("    SEND failed: %s\n", libusb_error_name(r)); return false; }
    printf("    SEND OK (%d bytes)\n", transferred);

    unsigned char recv_buf[256] = {0};
    transferred = 0;
    r = libusb_bulk_transfer(h, ep_in, recv_buf, sizeof(recv_buf), &transferred, 5000);
    if (r != 0) { printf("    RECV: %s\n", libusb_error_name(r)); return false; }

    printf("    RECV OK (%d bytes)\n", transferred);
    if (transferred >= HEADER_SIZE) {
        MiraHeader* ack = (MiraHeader*)recv_buf;
        printf("    magic=0x%08X cmd=0x%02X seq=%d\n", ack->magic, ack->cmd, ack->seq);
        if (ack->magic == MIRA_MAGIC && ack->cmd == CMD_ACK) {
            printf("    >>> ACK OK! <<<\n");
            return true;
        }
    }
    return false;
}

int main() {
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0) return 1;

    printf("=== AOA I/O Test v3 ===\n\n");

    int switched = switch_to_aoa(ctx);
    printf("Switched %d device(s)\n", switched);

    printf("Waiting 15s for re-enumeration + app startup...\n");
    for (int i = 1; i <= 15; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        printf("  %ds...\r", i); fflush(stdout);
    }
    printf("                \n\n");

    // Find ALL AOA devices
    libusb_device** devs;
    ssize_t cnt = libusb_get_device_list(ctx, &devs);
    int aoa_count = 0;
    int success_count = 0;

    for (ssize_t i = 0; i < cnt; i++) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(devs[i], &desc) != 0) continue;
        if (desc.idVendor != 0x18D1 || desc.idProduct < 0x2D00 || desc.idProduct > 0x2D05) continue;

        aoa_count++;
        uint8_t bus = libusb_get_bus_number(devs[i]);
        uint8_t addr = libusb_get_device_address(devs[i]);
        printf("[AOA #%d] PID=0x%04X bus=%d addr=%d\n", aoa_count, desc.idProduct, bus, addr);

        libusb_device_handle* h = nullptr;
        int r = libusb_open(devs[i], &h);
        if (r != 0) {
            printf("  OPEN FAILED: %s (skipping)\n\n", libusb_error_name(r));
            continue;
        }
        printf("  Opened OK\n");

        // Find bulk endpoints
        struct libusb_config_descriptor* config = nullptr;
        libusb_get_active_config_descriptor(devs[i], &config);
        uint8_t ep_in = 0, ep_out = 0;
        int claim_iface = -1;

        if (config) {
            for (int iface = 0; iface < config->bNumInterfaces; iface++) {
                const auto& alt = config->interface[iface].altsetting[0];
                if (alt.bInterfaceClass == 0xFF || alt.bNumEndpoints >= 2) {
                    for (int ep = 0; ep < alt.bNumEndpoints; ep++) {
                        uint8_t a = alt.endpoint[ep].bEndpointAddress;
                        if ((alt.endpoint[ep].bmAttributes & 0x03) == LIBUSB_TRANSFER_TYPE_BULK) {
                            if (a & 0x80) ep_in = a; else ep_out = a;
                        }
                    }
                    if (ep_in && ep_out) { claim_iface = iface; break; }
                }
            }
            libusb_free_config_descriptor(config);
        }

        if (!ep_in || !ep_out) {
            printf("  No bulk endpoints found (skipping)\n\n");
            libusb_close(h);
            continue;
        }

        printf("  EP_IN=0x%02X EP_OUT=0x%02X iface=%d\n", ep_in, ep_out, claim_iface);
        libusb_detach_kernel_driver(h, claim_iface);
        r = libusb_claim_interface(h, claim_iface);
        if (r != 0) {
            printf("  Claim interface failed: %s (skipping)\n\n", libusb_error_name(r));
            libusb_close(h);
            continue;
        }

        // Test PING
        printf("  PING seq=1:\n");
        bool ok = test_ping(h, ep_out, ep_in, 1);
        if (ok) {
            printf("  PING seq=2:\n");
            test_ping(h, ep_out, ep_in, 2);
            success_count++;
        }

        libusb_release_interface(h, claim_iface);
        libusb_close(h);
        printf("\n");
    }

    libusb_free_device_list(devs, 1);
    printf("=== Result: %d AOA found, %d responded ===\n", aoa_count, success_count);

    libusb_exit(ctx);
    return success_count > 0 ? 0 : 1;
}
