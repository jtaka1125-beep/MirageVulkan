// AOA I/O Test v4 - Full command test (PING, TAP, BACK, KEY)
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

#define CMD_PING  0
#define CMD_TAP   1
#define CMD_BACK  2
#define CMD_KEY   3
#define CMD_ACK   0x80

#pragma pack(push, 1)
struct MiraHeader {
    uint32_t magic;
    uint8_t  version;
    uint8_t  cmd;
    uint32_t seq;
    uint32_t payload_len;
};

struct TapPayload {
    int32_t x, y, w, h, flags;
};

struct KeyPayload {
    int32_t keycode, flags;
};
#pragma pack(pop)

struct AoaDevice {
    libusb_device_handle* handle;
    uint8_t ep_in, ep_out;
    int iface;
    uint8_t bus, addr;
};

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

static bool send_and_recv(AoaDevice& dev, uint8_t* pkt, int pkt_len, int seq) {
    int transferred = 0;
    int r = libusb_bulk_transfer(dev.handle, dev.ep_out, pkt, pkt_len, &transferred, 2000);
    if (r != 0) { printf("    SEND FAIL: %s\n", libusb_error_name(r)); return false; }

    unsigned char recv_buf[256] = {0};
    transferred = 0;
    r = libusb_bulk_transfer(dev.handle, dev.ep_in, recv_buf, sizeof(recv_buf), &transferred, 5000);
    if (r != 0) { printf("    RECV FAIL: %s\n", libusb_error_name(r)); return false; }

    if (transferred >= HEADER_SIZE) {
        MiraHeader* ack = (MiraHeader*)recv_buf;
        if (ack->magic == MIRA_MAGIC && ack->cmd == CMD_ACK && ack->seq == (uint32_t)seq) {
            uint8_t status = (transferred >= HEADER_SIZE + 5) ? recv_buf[HEADER_SIZE + 4] : 0xFF;
            printf("    ACK seq=%d status=%d %s\n", ack->seq, status, status == 0 ? "OK" : "ERR");
            return status == 0;
        }
    }
    printf("    Unexpected response (%d bytes)\n", transferred);
    return false;
}

int main() {
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0) return 1;

    printf("=== AOA Full Command Test ===\n\n");

    int switched = switch_to_aoa(ctx);
    printf("Switched %d device(s)\n", switched);

    printf("Waiting 15s...\n");
    for (int i = 1; i <= 15; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        printf("  %ds...\r", i); fflush(stdout);
    }
    printf("               \n\n");

    // Find and open all AOA devices
    libusb_device** devs;
    ssize_t cnt = libusb_get_device_list(ctx, &devs);
    std::vector<AoaDevice> aoa_devs;

    for (ssize_t i = 0; i < cnt; i++) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(devs[i], &desc) != 0) continue;
        if (desc.idVendor != 0x18D1 || desc.idProduct < 0x2D00 || desc.idProduct > 0x2D05) continue;

        AoaDevice ad = {};
        ad.bus = libusb_get_bus_number(devs[i]);
        ad.addr = libusb_get_device_address(devs[i]);

        if (libusb_open(devs[i], &ad.handle) != 0) continue;

        struct libusb_config_descriptor* config = nullptr;
        libusb_get_active_config_descriptor(devs[i], &config);
        if (config) {
            for (int iface = 0; iface < config->bNumInterfaces; iface++) {
                const auto& alt = config->interface[iface].altsetting[0];
                if (alt.bInterfaceClass == 0xFF || alt.bNumEndpoints >= 2) {
                    for (int ep = 0; ep < alt.bNumEndpoints; ep++) {
                        uint8_t a = alt.endpoint[ep].bEndpointAddress;
                        if ((alt.endpoint[ep].bmAttributes & 0x03) == LIBUSB_TRANSFER_TYPE_BULK) {
                            if (a & 0x80) ad.ep_in = a; else ad.ep_out = a;
                        }
                    }
                    if (ad.ep_in && ad.ep_out) { ad.iface = iface; break; }
                }
            }
            libusb_free_config_descriptor(config);
        }

        if (!ad.ep_in || !ad.ep_out) { libusb_close(ad.handle); continue; }

        libusb_detach_kernel_driver(ad.handle, ad.iface);
        if (libusb_claim_interface(ad.handle, ad.iface) != 0) { libusb_close(ad.handle); continue; }

        aoa_devs.push_back(ad);
    }

    printf("Found %zu AOA device(s)\n\n", aoa_devs.size());

    // Test each device
    int seq = 1;
    for (size_t d = 0; d < aoa_devs.size(); d++) {
        auto& dev = aoa_devs[d];
        printf("=== Device #%zu (bus=%d addr=%d) ===\n", d+1, dev.bus, dev.addr);

        // Test 1: PING
        printf("[1] PING:\n");
        MiraHeader hdr = {MIRA_MAGIC, MIRA_VERSION, CMD_PING, (uint32_t)seq, 0};
        bool ping_ok = send_and_recv(dev, (uint8_t*)&hdr, HEADER_SIZE, seq);
        seq++;

        // Test 2: TAP (center of 800x1340 screen)
        printf("[2] TAP (400,670):\n");
        uint8_t tap_pkt[HEADER_SIZE + sizeof(TapPayload)];
        MiraHeader* tap_hdr = (MiraHeader*)tap_pkt;
        *tap_hdr = {MIRA_MAGIC, MIRA_VERSION, CMD_TAP, (uint32_t)seq, sizeof(TapPayload)};
        TapPayload* tap = (TapPayload*)(tap_pkt + HEADER_SIZE);
        *tap = {400, 670, 800, 1340, 0};
        bool tap_ok = send_and_recv(dev, tap_pkt, sizeof(tap_pkt), seq);
        seq++;

        // Test 3: BACK
        printf("[3] BACK:\n");
        hdr = {MIRA_MAGIC, MIRA_VERSION, CMD_BACK, (uint32_t)seq, 0};
        bool back_ok = send_and_recv(dev, (uint8_t*)&hdr, HEADER_SIZE, seq);
        seq++;

        // Test 4: KEY (HOME = keycode 3)
        printf("[4] KEY (HOME=3):\n");
        uint8_t key_pkt[HEADER_SIZE + sizeof(KeyPayload)];
        MiraHeader* key_hdr = (MiraHeader*)key_pkt;
        *key_hdr = {MIRA_MAGIC, MIRA_VERSION, CMD_KEY, (uint32_t)seq, sizeof(KeyPayload)};
        KeyPayload* key = (KeyPayload*)(key_pkt + HEADER_SIZE);
        *key = {3, 0};  // KEYCODE_HOME
        bool key_ok = send_and_recv(dev, key_pkt, sizeof(key_pkt), seq);
        seq++;

        printf("  Results: PING=%s TAP=%s BACK=%s KEY=%s\n\n",
               ping_ok?"OK":"FAIL", tap_ok?"OK":"FAIL",
               back_ok?"OK":"FAIL", key_ok?"OK":"FAIL");

        libusb_release_interface(dev.handle, dev.iface);
        libusb_close(dev.handle);
    }

    libusb_free_device_list(devs, 1);
    libusb_exit(ctx);
    return 0;
}
