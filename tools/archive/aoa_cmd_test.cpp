// aoa_cmd_test.cpp - AOA TAP/BACK command test
// Sends TAP and BACK commands to already-connected AOA devices
// (Does NOT re-do AOA switch - devices must already be in AOA mode)

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>
#include <libusb-1.0/libusb.h>

#pragma pack(push, 1)
struct MiraHeader {
    uint32_t magic;     // 0x4D495241 "MIRA"
    uint8_t  version;
    uint8_t  cmd;
    uint16_t seq;
    uint32_t payload_len;
    // 12 bytes total
};
#pragma pack(pop)

// Commands
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

static bool send_cmd(AoaDev& dev, uint8_t cmd, uint16_t seq,
                     const uint8_t* payload, uint32_t payload_len) {
    MiraHeader hdr;
    hdr.magic = 0x4D495241;
    hdr.version = 1;
    hdr.cmd = cmd;
    hdr.seq = seq;
    hdr.payload_len = payload_len;

    std::vector<uint8_t> buf(sizeof(hdr) + payload_len);
    memcpy(buf.data(), &hdr, sizeof(hdr));
    if (payload && payload_len > 0)
        memcpy(buf.data() + sizeof(hdr), payload, payload_len);

    int transferred = 0;
    int r = libusb_bulk_transfer(dev.h, dev.ep_out, buf.data(), (int)buf.size(),
                                 &transferred, 2000);
    if (r != 0) {
        fprintf(stderr, "  SEND ERR: %s\n", libusb_error_name(r));
        return false;
    }
    printf("  SEND OK (%d bytes)\n", transferred);
    return true;
}

static bool recv_ack(AoaDev& dev, uint16_t expected_seq, int timeout_ms = 3000) {
    uint8_t buf[256];
    int transferred = 0;
    int r = libusb_bulk_transfer(dev.h, dev.ep_in, buf, sizeof(buf),
                                 &transferred, timeout_ms);
    if (r != 0) {
        fprintf(stderr, "  RECV ERR: %s\n", libusb_error_name(r));
        return false;
    }

    if (transferred >= (int)sizeof(MiraHeader)) {
        MiraHeader* h = (MiraHeader*)buf;
        if (h->magic == 0x4D495241 && h->cmd == CMD_ACK && h->seq == expected_seq) {
            printf("  ACK OK (seq=%d)\n", expected_seq);
            return true;
        }
        printf("  RECV: magic=0x%08X cmd=0x%02X seq=%d (unexpected)\n",
               h->magic, h->cmd, h->seq);
    }
    return false;
}

int main(int argc, char* argv[]) {
    printf("=== AOA Command Test (TAP/BACK) ===\n\n");

    // Parse args
    int tap_x = 400, tap_y = 700;
    if (argc >= 3) {
        tap_x = atoi(argv[1]);
        tap_y = atoi(argv[2]);
    }

    libusb_init(NULL);

    // Find AOA devices (already switched)
    libusb_device** devs;
    ssize_t cnt = libusb_get_device_list(NULL, &devs);
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

        // Get endpoints
        struct libusb_config_descriptor* config;
        libusb_get_active_config_descriptor(devs[i], &config);
        for (int j = 0; j < config->bNumInterfaces; j++) {
            const auto& iface = config->interface[j].altsetting[0];
            for (int k = 0; k < iface.bNumEndpoints; k++) {
                uint8_t ep = iface.endpoint[k].bEndpointAddress;
                if (ep & 0x80) d.ep_in = ep;
                else d.ep_out = ep;
            }
            if (d.ep_in && d.ep_out) {
                d.iface = j;
                break;
            }
        }
        libusb_free_config_descriptor(config);

        if (d.ep_in && d.ep_out) {
            libusb_claim_interface(d.h, d.iface);
            aoa_devs.push_back(d);
        } else {
            libusb_close(d.h);
        }
    }
    libusb_free_device_list(devs, 1);

    printf("Found %zu AOA device(s)\n\n", aoa_devs.size());
    if (aoa_devs.empty()) {
        printf("No AOA devices. Run aoa_io_test3 first.\n");
        return 1;
    }

    // Test on first device only
    auto& dev = aoa_devs[0];
    printf("Testing device bus=%d addr=%d\n", dev.bus, dev.addr);
    uint16_t seq = 1;

    // 1. PING
    printf("\n[1] PING\n");
    send_cmd(dev, CMD_PING, seq, nullptr, 0);
    recv_ack(dev, seq);
    seq++;

    // 2. TAP
    printf("\n[2] TAP x=%d y=%d\n", tap_x, tap_y);
    struct { int32_t x; int32_t y; } tap_payload = { tap_x, tap_y };
    send_cmd(dev, CMD_TAP, seq, (uint8_t*)&tap_payload, sizeof(tap_payload));
    recv_ack(dev, seq);
    seq++;

    // Wait and see
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // 3. BACK
    printf("\n[3] BACK\n");
    send_cmd(dev, CMD_BACK, seq, nullptr, 0);
    recv_ack(dev, seq);
    seq++;

    // Wait
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 4. Another PING to confirm still alive
    printf("\n[4] PING (confirm alive)\n");
    send_cmd(dev, CMD_PING, seq, nullptr, 0);
    recv_ack(dev, seq);

    // Cleanup
    for (auto& d : aoa_devs) {
        libusb_release_interface(d.h, d.iface);
        libusb_close(d.h);
    }
    libusb_exit(NULL);

    printf("\n=== Done ===\n");
    return 0;
}
