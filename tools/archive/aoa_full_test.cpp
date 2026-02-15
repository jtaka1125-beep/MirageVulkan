// aoa_full_test.cpp - Complete AOA test: switch + PING + TAP + BACK
// Single program that does everything

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>
#include <libusb-1.0/libusb.h>

#pragma pack(push, 1)
struct MiraHeader {
    uint32_t magic;
    uint8_t  version;
    uint8_t  cmd;
    uint32_t seq;
    uint32_t payload_len;
};
#pragma pack(pop)

enum {
    CMD_PING = 0x00,
    CMD_TAP  = 0x01,
    CMD_BACK = 0x02,
    CMD_KEY  = 0x03,
    CMD_ACK  = 0x80,
};

struct AoaDev {
    libusb_device_handle* h;
    uint8_t ep_in;
    uint8_t ep_out;
    int iface;
    int bus;
    int addr;
};

static bool send_cmd(AoaDev& dev, uint8_t cmd, uint32_t seq,
                     const uint8_t* payload, uint32_t plen) {
    MiraHeader hdr = { 0x4D495241, 1, cmd, seq, plen };
    std::vector<uint8_t> buf(sizeof(hdr) + plen);
    memcpy(buf.data(), &hdr, sizeof(hdr));
    if (payload && plen > 0)
        memcpy(buf.data() + sizeof(hdr), payload, plen);

    int xfer = 0;
    int r = libusb_bulk_transfer(dev.h, dev.ep_out, buf.data(), (int)buf.size(), &xfer, 2000);
    if (r != 0) { fprintf(stderr, "    SEND ERR: %s\n", libusb_error_name(r)); return false; }
    printf("    SEND OK (%d bytes)\n", xfer);
    return true;
}

static bool recv_ack(AoaDev& dev, uint32_t expected_seq, int timeout_ms = 3000) {
    uint8_t buf[256];
    int xfer = 0;
    int r = libusb_bulk_transfer(dev.h, dev.ep_in, buf, sizeof(buf), &xfer, timeout_ms);
    if (r != 0) { fprintf(stderr, "    RECV ERR: %s\n", libusb_error_name(r)); return false; }
    if (xfer >= (int)sizeof(MiraHeader)) {
        MiraHeader* h = (MiraHeader*)buf;
        if (h->magic == 0x4D495241 && h->cmd == CMD_ACK && h->seq == expected_seq) {
            printf("    ACK OK (seq=%d, payload=%d bytes)\n", expected_seq, h->payload_len);
            return true;
        }
    }
    return false;
}

static int switchToAoa(libusb_context* ctx) {
    libusb_device** devs;
    ssize_t cnt = libusb_get_device_list(ctx, &devs);
    int switched = 0;

    for (ssize_t i = 0; i < cnt; i++) {
        struct libusb_device_descriptor desc;
        libusb_get_device_descriptor(devs[i], &desc);
        // Skip non-Android and already-AOA
        if (desc.idVendor == 0x18D1 && desc.idProduct >= 0x2D00 && desc.idProduct <= 0x2D05) continue;
        if (desc.bDeviceClass == 9) continue; // hub

        libusb_device_handle* h;
        if (libusb_open(devs[i], &h) != 0) continue;

        // Check AOA protocol version
        uint8_t ver_buf[2] = {0};
        int r = libusb_control_transfer(h, 0xC0, 51, 0, 0, ver_buf, 2, 1000);
        if (r < 0) { libusb_close(h); continue; }

        uint16_t ver = ver_buf[0] | (ver_buf[1] << 8);
        if (ver == 0) { libusb_close(h); continue; }

        // Send AOA strings
        const char* strings[] = { "Mirage", "MirageCtl", "Mirage Control", "1", "https://github.com/mirage", "MirageCtl001" };
        for (int s = 0; s < 6; s++) {
            libusb_control_transfer(h, 0x40, 52, 0, s, (uint8_t*)strings[s], strlen(strings[s])+1, 1000);
        }

        // Start AOA
        libusb_control_transfer(h, 0x40, 53, 0, 0, NULL, 0, 1000);
        libusb_close(h);
        switched++;
    }
    libusb_free_device_list(devs, 1);
    return switched;
}

int main(int argc, char* argv[]) {
    printf("=== AOA Full Test (Switch + PING + TAP + BACK) ===\n\n");

    int tap_x = 400, tap_y = 700;
    if (argc >= 3) {
        tap_x = atoi(argv[1]);
        tap_y = atoi(argv[2]);
    }
    printf("TAP target: (%d, %d)\n\n", tap_x, tap_y);

    libusb_context* ctx;
    libusb_init(&ctx);

    // Phase 1: Switch to AOA
    printf("[Phase 1] AOA Switch\n");
    int sw = switchToAoa(ctx);
    printf("  Switched %d device(s)\n", sw);

    // Phase 2: Wait for re-enumeration
    printf("\n[Phase 2] Waiting 15s for re-enumeration...\n");
    for (int i = 1; i <= 15; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        printf("  %ds...\n", i);
    }

    // Phase 3: Find AOA devices
    printf("\n[Phase 3] Finding AOA devices\n");
    libusb_device** devs;
    ssize_t cnt = libusb_get_device_list(ctx, &devs);
    std::vector<AoaDev> aoa_devs;

    for (ssize_t i = 0; i < cnt; i++) {
        struct libusb_device_descriptor desc;
        libusb_get_device_descriptor(devs[i], &desc);
        if (desc.idVendor != 0x18D1) continue;
        if (desc.idProduct < 0x2D00 || desc.idProduct > 0x2D05) continue;

        AoaDev d = {};
        d.bus = libusb_get_bus_number(devs[i]);
        d.addr = libusb_get_device_address(devs[i]);

        if (libusb_open(devs[i], &d.h) != 0) continue;

        struct libusb_config_descriptor* config;
        libusb_get_active_config_descriptor(devs[i], &config);
        for (int j = 0; j < config->bNumInterfaces; j++) {
            const auto& iface = config->interface[j].altsetting[0];
            for (int k = 0; k < iface.bNumEndpoints; k++) {
                uint8_t ep = iface.endpoint[k].bEndpointAddress;
                if (ep & 0x80) d.ep_in = ep;
                else d.ep_out = ep;
            }
            if (d.ep_in && d.ep_out) { d.iface = j; break; }
        }
        libusb_free_config_descriptor(config);

        if (d.ep_in && d.ep_out) {
            libusb_claim_interface(d.h, d.iface);
            aoa_devs.push_back(d);
            printf("  AOA device: bus=%d addr=%d PID=0x%04X\n", d.bus, d.addr, desc.idProduct);
        } else {
            libusb_close(d.h);
        }
    }
    libusb_free_device_list(devs, 1);
    printf("  Found %zu AOA device(s)\n", aoa_devs.size());

    if (aoa_devs.empty()) {
        printf("\nNo AOA devices found!\n");
        libusb_exit(ctx);
        return 1;
    }

    // Phase 4: Test commands on first device
    auto& dev = aoa_devs[0];
    printf("\n[Phase 4] Testing commands on bus=%d addr=%d\n", dev.bus, dev.addr);
    uint32_t seq = 1;

    // PING
    printf("\n  [1] PING\n");
    send_cmd(dev, CMD_PING, seq, nullptr, 0);
    bool ping_ok = recv_ack(dev, seq);
    seq++;

    if (!ping_ok) {
        printf("\n  PING failed - device not responding. Aborting.\n");
    } else {
        // TAP
        printf("\n  [2] TAP (%d, %d)\n", tap_x, tap_y);
        struct { int32_t x; int32_t y; } tap_pl = { tap_x, tap_y };
        send_cmd(dev, CMD_TAP, seq, (uint8_t*)&tap_pl, sizeof(tap_pl));
        recv_ack(dev, seq);
        seq++;

        std::this_thread::sleep_for(std::chrono::milliseconds(1500));

        // BACK
        printf("\n  [3] BACK\n");
        send_cmd(dev, CMD_BACK, seq, nullptr, 0);
        recv_ack(dev, seq);
        seq++;

        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Final PING
        printf("\n  [4] PING (confirm alive)\n");
        send_cmd(dev, CMD_PING, seq, nullptr, 0);
        recv_ack(dev, seq);
    }

    // Cleanup
    for (auto& d : aoa_devs) {
        libusb_release_interface(d.h, d.iface);
        libusb_close(d.h);
    }
    libusb_exit(ctx);

    printf("\n=== Done ===\n");
    return 0;
}
