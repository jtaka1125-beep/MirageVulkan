// aoa_all_test.cpp - Test ALL AOA devices with PING + TAP + BACK
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

enum { CMD_PING=0, CMD_TAP=1, CMD_BACK=2, CMD_KEY=3, CMD_ACK=0x80 };

struct AoaDev {
    libusb_device_handle* h;
    uint8_t ep_in, ep_out;
    int iface, bus, addr;
};

static bool send_cmd(AoaDev& d, uint8_t cmd, uint32_t seq, const uint8_t* pl, uint32_t plen) {
    MiraHeader hdr = {0x4D495241, 1, cmd, seq, plen};
    std::vector<uint8_t> buf(sizeof(hdr)+plen);
    memcpy(buf.data(), &hdr, sizeof(hdr));
    if (pl && plen) memcpy(buf.data()+sizeof(hdr), pl, plen);
    int xfer=0;
    int r = libusb_bulk_transfer(d.h, d.ep_out, buf.data(), (int)buf.size(), &xfer, 2000);
    if (r != 0) { printf("      SEND ERR: %s\n", libusb_error_name(r)); return false; }
    return true;
}

static bool recv_ack(AoaDev& d, uint32_t seq, int timeout=3000) {
    uint8_t buf[256]; int xfer=0;
    int r = libusb_bulk_transfer(d.h, d.ep_in, buf, sizeof(buf), &xfer, timeout);
    if (r != 0) { printf("      RECV ERR: %s\n", libusb_error_name(r)); return false; }
    if (xfer >= (int)sizeof(MiraHeader)) {
        MiraHeader* h = (MiraHeader*)buf;
        if (h->magic==0x4D495241 && h->cmd==CMD_ACK && h->seq==seq) return true;
    }
    return false;
}

static void send_aoa_string(libusb_device_handle* h, uint16_t idx, const char* s) {
    libusb_control_transfer(h, 0x40, 52, 0, idx, (uint8_t*)s, strlen(s)+1, 1000);
}

int main(int argc, char* argv[]) {
    int tap_x=400, tap_y=700;
    if (argc>=3) { tap_x=atoi(argv[1]); tap_y=atoi(argv[2]); }

    printf("=== AOA All-Device Test ===\n");
    printf("TAP: (%d,%d)\n\n", tap_x, tap_y);

    libusb_context* ctx;
    libusb_init(&ctx);

    // Phase 1: Switch
    printf("[Phase 1] AOA Switch\n");
    libusb_device** devs;
    ssize_t cnt = libusb_get_device_list(ctx, &devs);
    int switched=0;
    for (ssize_t i=0; i<cnt; i++) {
        libusb_device_descriptor desc;
        libusb_get_device_descriptor(devs[i], &desc);
        if (desc.idVendor==0x18D1 && desc.idProduct>=0x2D00 && desc.idProduct<=0x2D05) continue;
        if (desc.bDeviceClass==9) continue;
        libusb_device_handle* h;
        if (libusb_open(devs[i], &h)!=0) continue;
        uint8_t ver[2]={0};
        if (libusb_control_transfer(h, 0xC0, 51, 0, 0, ver, 2, 1000)<0) { libusb_close(h); continue; }
        if ((ver[0]|(ver[1]<<8))==0) { libusb_close(h); continue; }
        send_aoa_string(h, 0, "Mirage");
        send_aoa_string(h, 1, "MirageCtl");
        send_aoa_string(h, 2, "Mirage Control");
        send_aoa_string(h, 3, "1");
        send_aoa_string(h, 4, "https://github.com/mirage");
        send_aoa_string(h, 5, "MirageCtl001");
        if (libusb_control_transfer(h, 0x40, 53, 0, 0, NULL, 0, 1000)>=0) switched++;
        libusb_close(h);
    }
    libusb_free_device_list(devs, 1);
    printf("  Switched %d device(s)\n", switched);

    // Phase 2: Wait
    printf("\n[Phase 2] Waiting 15s...\n");
    for (int i=1; i<=15; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        printf("  %ds...\r", i); fflush(stdout);
    }
    printf("                \n");

    // Phase 3: Find & test ALL
    printf("\n[Phase 3] Find and test ALL AOA devices\n\n");
    cnt = libusb_get_device_list(ctx, &devs);
    int found=0, success=0;

    for (ssize_t i=0; i<cnt; i++) {
        libusb_device_descriptor desc;
        libusb_get_device_descriptor(devs[i], &desc);
        if (desc.idVendor!=0x18D1 || desc.idProduct<0x2D00 || desc.idProduct>0x2D05) continue;
        found++;

        AoaDev d={};
        d.bus = libusb_get_bus_number(devs[i]);
        d.addr = libusb_get_device_address(devs[i]);
        printf("[Device #%d] bus=%d addr=%d PID=0x%04X\n", found, d.bus, d.addr, desc.idProduct);

        if (libusb_open(devs[i], &d.h)!=0) { printf("  OPEN FAILED\n\n"); continue; }

        // Get endpoints
        libusb_config_descriptor* config;
        libusb_get_active_config_descriptor(devs[i], &config);
        for (int j=0; j<config->bNumInterfaces; j++) {
            const auto& alt = config->interface[j].altsetting[0];
            for (int k=0; k<alt.bNumEndpoints; k++) {
                uint8_t ep = alt.endpoint[k].bEndpointAddress;
                if ((alt.endpoint[k].bmAttributes&3)==LIBUSB_TRANSFER_TYPE_BULK) {
                    if (ep&0x80) d.ep_in=ep; else d.ep_out=ep;
                }
            }
            if (d.ep_in && d.ep_out) { d.iface=j; break; }
        }
        libusb_free_config_descriptor(config);

        if (!d.ep_in || !d.ep_out) { printf("  No endpoints\n\n"); libusb_close(d.h); continue; }

        libusb_detach_kernel_driver(d.h, d.iface);
        libusb_claim_interface(d.h, d.iface);

        uint32_t seq=1;
        bool ok = false;

        // PING
        printf("  PING: ");
        send_cmd(d, CMD_PING, seq, nullptr, 0);
        ok = recv_ack(d, seq);
        printf("%s\n", ok ? "ACK OK" : "TIMEOUT");
        seq++;

        if (ok) {
            // TAP
            printf("  TAP(%d,%d): ", tap_x, tap_y);
            struct { int32_t x,y; } tap_pl = {tap_x, tap_y};
            send_cmd(d, CMD_TAP, seq, (uint8_t*)&tap_pl, sizeof(tap_pl));
            bool tap_ok = recv_ack(d, seq);
            printf("%s\n", tap_ok ? "ACK OK" : "TIMEOUT");
            seq++;

            std::this_thread::sleep_for(std::chrono::milliseconds(1000));

            // BACK
            printf("  BACK: ");
            send_cmd(d, CMD_BACK, seq, nullptr, 0);
            bool back_ok = recv_ack(d, seq);
            printf("%s\n", back_ok ? "ACK OK" : "TIMEOUT");
            seq++;

            success++;
        }

        libusb_release_interface(d.h, d.iface);
        libusb_close(d.h);
        printf("\n");
    }
    libusb_free_device_list(devs, 1);

    printf("=== Result: %d found, %d responding ===\n", found, success);
    libusb_exit(ctx);
    return 0;
}
