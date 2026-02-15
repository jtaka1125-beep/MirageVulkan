// AOA Data I/O Test - Send PING, receive ACK
// Protocol: MIRA header (14 bytes) + payload
// Must run with adb kill-server first!
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>
#include <libusb-1.0/libusb.h>

#define AOA_GET_PROTOCOL    51
#define AOA_SEND_STRING     52
#define AOA_START_ACCESSORY 53

// Mirage Protocol constants (matches Protocol.kt)
#define MIRA_MAGIC    0x4D495241  // "MIRA" LE
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
    return libusb_control_transfer(h,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
        AOA_SEND_STRING, 0, idx,
        (unsigned char*)s, strlen(s)+1, 1000) >= 0;
}

// Phase 1: Switch all MediaTek devices to AOA
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
            AOA_GET_PROTOCOL, 0, 0, ver, 2, 1000) < 0) {
            libusb_close(h); continue;
        }

        send_aoa_string(h, 0, "Mirage");
        send_aoa_string(h, 1, "MirageCtl");
        send_aoa_string(h, 2, "Mirage Control");
        send_aoa_string(h, 3, "1");
        send_aoa_string(h, 4, "https://github.com/mirage");
        send_aoa_string(h, 5, "MirageCtl001");

        if (libusb_control_transfer(h, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
            AOA_START_ACCESSORY, 0, 0, nullptr, 0, 1000) >= 0)
            switched++;

        libusb_close(h);
    }
    libusb_free_device_list(devs, 1);
    return switched;
}

// Phase 2: Find AOA devices and test I/O on the first one
static bool test_aoa_io(libusb_context* ctx) {
    libusb_device** devs;
    ssize_t cnt = libusb_get_device_list(ctx, &devs);
    libusb_device* aoa_dev = nullptr;

    for (ssize_t i = 0; i < cnt; i++) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(devs[i], &desc) != 0) continue;
        if (desc.idVendor == 0x18D1 && desc.idProduct >= 0x2D00 && desc.idProduct <= 0x2D05) {
            aoa_dev = devs[i];
            printf("Found AOA device: VID=%04x PID=%04x\n", desc.idVendor, desc.idProduct);
            break;
        }
    }

    if (!aoa_dev) {
        printf("No AOA device found!\n");
        libusb_free_device_list(devs, 1);
        return false;
    }

    libusb_device_handle* h = nullptr;
    int r = libusb_open(aoa_dev, &h);
    if (r != 0) {
        printf("Failed to open AOA device: %s\n", libusb_error_name(r));
        libusb_free_device_list(devs, 1);
        return false;
    }

    // Find bulk endpoints by examining interface descriptor
    struct libusb_config_descriptor* config = nullptr;
    libusb_get_active_config_descriptor(aoa_dev, &config);

    uint8_t ep_in = 0, ep_out = 0;
    if (config) {
        for (int iface = 0; iface < config->bNumInterfaces; iface++) {
            const auto& alt = config->interface[iface].altsetting[0];
            // AOA interface: class 0xFF (vendor-specific)
            if (alt.bInterfaceClass == 0xFF || alt.bNumEndpoints >= 2) {
                for (int ep = 0; ep < alt.bNumEndpoints; ep++) {
                    uint8_t addr = alt.endpoint[ep].bEndpointAddress;
                    uint8_t attr = alt.endpoint[ep].bmAttributes;
                    if ((attr & 0x03) == LIBUSB_TRANSFER_TYPE_BULK) {
                        if (addr & 0x80) ep_in = addr;
                        else ep_out = addr;
                    }
                }
                if (ep_in && ep_out) {
                    printf("Using interface %d: EP_IN=0x%02x EP_OUT=0x%02x\n", iface, ep_in, ep_out);
                    // Claim this interface
                    libusb_detach_kernel_driver(h, iface);
                    libusb_claim_interface(h, iface);
                    break;
                }
            }
        }
        libusb_free_config_descriptor(config);
    }

    if (!ep_in || !ep_out) {
        printf("Could not find bulk endpoints!\n");
        libusb_close(h);
        libusb_free_device_list(devs, 1);
        return false;
    }

    // Build PING packet
    MiraHeader hdr;
    hdr.magic = MIRA_MAGIC;
    hdr.version = MIRA_VERSION;
    hdr.cmd = CMD_PING;
    hdr.seq = 1;
    hdr.payload_len = 0;

    printf("\n=== Sending PING (seq=1) ===\n");
    int transferred = 0;
    r = libusb_bulk_transfer(h, ep_out, (unsigned char*)&hdr, HEADER_SIZE, &transferred, 2000);
    if (r != 0) {
        printf("SEND failed: %s\n", libusb_error_name(r));
    } else {
        printf("SEND OK: %d bytes\n", transferred);
    }

    // Wait for ACK
    printf("Waiting for ACK...\n");
    unsigned char recv_buf[256] = {0};
    transferred = 0;
    r = libusb_bulk_transfer(h, ep_in, recv_buf, sizeof(recv_buf), &transferred, 5000);
    if (r != 0) {
        printf("RECV failed: %s (timeout or error)\n", libusb_error_name(r));
    } else {
        printf("RECV OK: %d bytes\n", transferred);
        if (transferred >= HEADER_SIZE) {
            MiraHeader* ack = (MiraHeader*)recv_buf;
            printf("  magic=0x%08X version=%d cmd=0x%02X seq=%d payload_len=%d\n",
                   ack->magic, ack->version, ack->cmd, ack->seq, ack->payload_len);
            if (ack->magic == MIRA_MAGIC && ack->cmd == CMD_ACK) {
                printf("  >>> ACK RECEIVED! Protocol working! <<<\n");
                if (transferred >= HEADER_SIZE + 5) {
                    uint8_t status = recv_buf[HEADER_SIZE + 4];
                    printf("  ACK status: %d (%s)\n", status, status == 0 ? "OK" : "ERROR");
                }
            }
        } else {
            printf("  Raw: ");
            for (int i = 0; i < transferred; i++) printf("%02X ", recv_buf[i]);
            printf("\n");
        }
    }

    // Send PING #2 to confirm
    hdr.seq = 2;
    printf("\n=== Sending PING (seq=2) ===\n");
    r = libusb_bulk_transfer(h, ep_out, (unsigned char*)&hdr, HEADER_SIZE, &transferred, 2000);
    printf("SEND: %s (%d bytes)\n", r == 0 ? "OK" : libusb_error_name(r), transferred);

    transferred = 0;
    r = libusb_bulk_transfer(h, ep_in, recv_buf, sizeof(recv_buf), &transferred, 5000);
    if (r == 0 && transferred >= HEADER_SIZE) {
        MiraHeader* ack = (MiraHeader*)recv_buf;
        printf("RECV: cmd=0x%02X seq=%d -> %s\n",
               ack->cmd, ack->seq,
               (ack->magic == MIRA_MAGIC && ack->cmd == CMD_ACK) ? "ACK OK!" : "unexpected");
    } else {
        printf("RECV: %s\n", r == 0 ? "short read" : libusb_error_name(r));
    }

    libusb_close(h);
    libusb_free_device_list(devs, 1);
    return true;
}

int main() {
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb init failed\n"); return 1; }

    printf("=== AOA Data I/O Test ===\n\n");

    // Phase 1: Switch to AOA
    printf("Phase 1: AOA switch...\n");
    int switched = switch_to_aoa(ctx);
    printf("Switched %d device(s)\n\n", switched);

    if (switched == 0) {
        printf("Checking if already in AOA mode...\n");
    }

    // Wait for re-enumeration (10 seconds based on test results)
    printf("Phase 2: Waiting 12s for re-enumeration + app startup...\n");
    for (int i = 1; i <= 12; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        printf("  %ds...\r", i); fflush(stdout);
    }
    printf("            \n");

    // Phase 3: Test I/O
    printf("Phase 3: Data I/O test...\n");
    bool ok = test_aoa_io(ctx);

    printf("\n=== Result: %s ===\n", ok ? "SUCCESS" : "FAILED");

    libusb_exit(ctx);
    return ok ? 0 : 1;
}
