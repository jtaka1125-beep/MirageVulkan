// aoa_all_test3.cpp - Fixed TAP payload (x,y,w,h,pressure = 20 bytes)
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
struct TapPayload {
    int32_t x, y, w, h, pressure; // 20 bytes
};
#pragma pack(pop)

enum { CMD_PING=0, CMD_TAP=1, CMD_BACK=2, CMD_ACK=0x80 };

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
    if (r != 0) { printf("    SEND ERR: %s\n", libusb_error_name(r)); return false; }
    return true;
}

static bool recv_ack(AoaDev& d, uint32_t seq, int timeout=3000) {
    uint8_t buf[256]; int xfer=0;
    int r = libusb_bulk_transfer(d.h, d.ep_in, buf, sizeof(buf), &xfer, timeout);
    if (r != 0) { printf("    RECV: %s\n", libusb_error_name(r)); return false; }
    if (xfer >= (int)sizeof(MiraHeader)) {
        MiraHeader* h = (MiraHeader*)buf;
        if (h->magic==0x4D495241 && h->cmd==CMD_ACK && h->seq==seq) return true;
    }
    return false;
}

static void aoa_str(libusb_device_handle* h, uint16_t i, const char* s) {
    libusb_control_transfer(h, 0x40, 52, 0, i, (uint8_t*)s, strlen(s)+1, 1000);
}

int main(int argc, char* argv[]) {
    int tx=400, ty=700;
    if (argc>=3) { tx=atoi(argv[1]); ty=atoi(argv[2]); }
    printf("=== AOA Test v3 (fixed TAP payload) ===\nTAP: (%d,%d)\n\n", tx, ty);

    libusb_context* ctx; libusb_init(&ctx);

    // Switch
    printf("[1] Switch\n");
    libusb_device** devs; ssize_t cnt=libusb_get_device_list(ctx,&devs); int sw=0;
    for (ssize_t i=0;i<cnt;i++) {
        libusb_device_descriptor desc; libusb_get_device_descriptor(devs[i],&desc);
        if (desc.idVendor==0x18D1&&desc.idProduct>=0x2D00&&desc.idProduct<=0x2D05) continue;
        if (desc.bDeviceClass==9) continue;
        libusb_device_handle* h; if (libusb_open(devs[i],&h)!=0) continue;
        uint8_t v[2]={0};
        if (libusb_control_transfer(h,0xC0,51,0,0,v,2,1000)<0){libusb_close(h);continue;}
        if ((v[0]|(v[1]<<8))==0){libusb_close(h);continue;}
        aoa_str(h,0,"Mirage"); aoa_str(h,1,"MirageCtl"); aoa_str(h,2,"Mirage Control");
        aoa_str(h,3,"1"); aoa_str(h,4,"https://github.com/mirage"); aoa_str(h,5,"MirageCtl001");
        if (libusb_control_transfer(h,0x40,53,0,0,NULL,0,1000)>=0) sw++;
        libusb_close(h);
    }
    libusb_free_device_list(devs,1);
    printf("  %d switched\n\n[2] Wait 15s\n", sw);
    for (int i=1;i<=15;i++){std::this_thread::sleep_for(std::chrono::seconds(1));printf("  %ds\r",i);fflush(stdout);}
    printf("       \n\n[3] Test\n\n");

    cnt=libusb_get_device_list(ctx,&devs); int found=0,ok_count=0;
    for (ssize_t i=0;i<cnt;i++) {
        libusb_device_descriptor desc; libusb_get_device_descriptor(devs[i],&desc);
        if (desc.idVendor!=0x18D1||desc.idProduct<0x2D00||desc.idProduct>0x2D05) continue;
        found++;
        AoaDev d={}; d.bus=libusb_get_bus_number(devs[i]); d.addr=libusb_get_device_address(devs[i]);
        printf("[#%d] bus=%d addr=%d\n", found, d.bus, d.addr);
        if (libusb_open(devs[i],&d.h)!=0){printf("  FAIL\n\n");continue;}
        libusb_config_descriptor* cfg; libusb_get_active_config_descriptor(devs[i],&cfg);
        for (int j=0;j<cfg->bNumInterfaces;j++){
            auto& a=cfg->interface[j].altsetting[0];
            for (int k=0;k<a.bNumEndpoints;k++){
                uint8_t ep=a.endpoint[k].bEndpointAddress;
                if ((a.endpoint[k].bmAttributes&3)==2){if(ep&0x80)d.ep_in=ep;else d.ep_out=ep;}
            }
            if (d.ep_in&&d.ep_out){d.iface=j;break;}
        }
        libusb_free_config_descriptor(cfg);
        if (!d.ep_in||!d.ep_out){libusb_close(d.h);continue;}
        libusb_detach_kernel_driver(d.h,d.iface);
        libusb_claim_interface(d.h,d.iface);

        uint32_t seq=1;
        printf("  PING: "); send_cmd(d,CMD_PING,seq,nullptr,0);
        bool ok=recv_ack(d,seq); printf("%s\n",ok?"OK":"FAIL"); seq++;
        if (ok) {
            TapPayload tap={tx,ty,800,1280,100};
            printf("  TAP:  "); send_cmd(d,CMD_TAP,seq,(uint8_t*)&tap,sizeof(tap));
            bool t=recv_ack(d,seq); printf("%s\n",t?"OK":"FAIL"); seq++;
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            printf("  BACK: "); send_cmd(d,CMD_BACK,seq,nullptr,0);
            bool b=recv_ack(d,seq); printf("%s\n",b?"OK":"FAIL"); seq++;
            ok_count++;
        }
        libusb_release_interface(d.h,d.iface); libusb_close(d.h); printf("\n");
    }
    libusb_free_device_list(devs,1);
    printf("=== %d found, %d OK ===\n", found, ok_count);
    libusb_exit(ctx); return 0;
}
