// aoa_stress_test.cpp - Stability test: rapid commands + sustained connection
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>
#include <libusb-1.0/libusb.h>

#pragma pack(push, 1)
struct MiraHeader { uint32_t magic; uint8_t version; uint8_t cmd; uint32_t seq; uint32_t payload_len; };
struct TapPayload { int32_t x,y,w,h,pressure; };
#pragma pack(pop)

enum { CMD_PING=0, CMD_TAP=1, CMD_BACK=2, CMD_ACK=0x80 };

struct AoaDev {
    libusb_device_handle* h; uint8_t ep_in,ep_out; int iface,bus,addr;
};

static bool send_cmd(AoaDev& d, uint8_t cmd, uint32_t seq, const uint8_t* pl, uint32_t plen) {
    MiraHeader hdr={0x4D495241,1,cmd,seq,plen};
    std::vector<uint8_t> buf(sizeof(hdr)+plen);
    memcpy(buf.data(),&hdr,sizeof(hdr));
    if(pl&&plen) memcpy(buf.data()+sizeof(hdr),pl,plen);
    int xfer=0;
    return libusb_bulk_transfer(d.h,d.ep_out,buf.data(),(int)buf.size(),&xfer,2000)==0;
}

static bool recv_ack(AoaDev& d, uint32_t seq, int timeout=3000) {
    uint8_t buf[256]; int xfer=0;
    int r=libusb_bulk_transfer(d.h,d.ep_in,buf,sizeof(buf),&xfer,timeout);
    if(r!=0) return false;
    if(xfer>=(int)sizeof(MiraHeader)) {
        MiraHeader* h=(MiraHeader*)buf;
        if(h->magic==0x4D495241 && h->cmd==CMD_ACK && h->seq==seq) return true;
    }
    return false;
}

static void aoa_str(libusb_device_handle* h, uint16_t i, const char* s) {
    libusb_control_transfer(h,0x40,52,0,i,(uint8_t*)s,strlen(s)+1,1000);
}

int main() {
    printf("=== AOA Stress Test ===\n\n");
    libusb_context* ctx; libusb_init(&ctx);

    // Switch
    printf("[1] Switch\n");
    libusb_device** devs; ssize_t cnt=libusb_get_device_list(ctx,&devs); int sw=0;
    for(ssize_t i=0;i<cnt;i++){
        libusb_device_descriptor desc; libusb_get_device_descriptor(devs[i],&desc);
        if(desc.idVendor==0x18D1&&desc.idProduct>=0x2D00&&desc.idProduct<=0x2D05) continue;
        if(desc.bDeviceClass==9) continue;
        libusb_device_handle* h; if(libusb_open(devs[i],&h)!=0) continue;
        uint8_t v[2]={0};
        if(libusb_control_transfer(h,0xC0,51,0,0,v,2,1000)<0){libusb_close(h);continue;}
        if((v[0]|(v[1]<<8))==0){libusb_close(h);continue;}
        aoa_str(h,0,"Mirage");aoa_str(h,1,"MirageCtl");aoa_str(h,2,"Mirage Control");
        aoa_str(h,3,"1");aoa_str(h,4,"https://github.com/mirage");aoa_str(h,5,"MirageCtl001");
        if(libusb_control_transfer(h,0x40,53,0,0,NULL,0,1000)>=0) sw++;
        libusb_close(h);
    }
    libusb_free_device_list(devs,1);
    printf("  %d switched\n",sw);

    printf("[2] Wait 15s\n");
    for(int i=1;i<=15;i++){std::this_thread::sleep_for(std::chrono::seconds(1));printf("  %d\r",i);fflush(stdout);}
    printf("       \n");

    // Find AOA devices
    printf("[3] Find AOA\n");
    cnt=libusb_get_device_list(ctx,&devs);
    std::vector<AoaDev> aoa;
    for(ssize_t i=0;i<cnt;i++){
        libusb_device_descriptor desc; libusb_get_device_descriptor(devs[i],&desc);
        if(desc.idVendor!=0x18D1||desc.idProduct<0x2D00||desc.idProduct>0x2D05) continue;
        AoaDev d={}; d.bus=libusb_get_bus_number(devs[i]); d.addr=libusb_get_device_address(devs[i]);
        if(libusb_open(devs[i],&d.h)!=0) continue;
        libusb_config_descriptor* cfg; libusb_get_active_config_descriptor(devs[i],&cfg);
        for(int j=0;j<cfg->bNumInterfaces;j++){
            auto& a=cfg->interface[j].altsetting[0];
            for(int k=0;k<a.bNumEndpoints;k++){
                uint8_t ep=a.endpoint[k].bEndpointAddress;
                if((a.endpoint[k].bmAttributes&3)==2){if(ep&0x80)d.ep_in=ep;else d.ep_out=ep;}
            }
            if(d.ep_in&&d.ep_out){d.iface=j;break;}
        }
        libusb_free_config_descriptor(cfg);
        if(!d.ep_in||!d.ep_out){libusb_close(d.h);continue;}
        libusb_detach_kernel_driver(d.h,d.iface);
        libusb_claim_interface(d.h,d.iface);
        aoa.push_back(d);
        printf("  dev bus=%d addr=%d\n",d.bus,d.addr);
    }
    libusb_free_device_list(devs,1);
    printf("  Found %zu\n\n",(size_t)aoa.size());

    if(aoa.empty()){printf("No devices!\n");libusb_exit(ctx);return 1;}

    // Test 1: Rapid PING (50 rounds per device)
    printf("[Test 1] Rapid PING x50 per device\n");
    for(size_t di=0;di<aoa.size();di++){
        auto& d=aoa[di];
        int ok=0,fail=0;
        uint32_t seq=1;
        for(int i=0;i<50;i++){
            if(send_cmd(d,CMD_PING,seq,nullptr,0)&&recv_ack(d,seq)) ok++;
            else fail++;
            seq++;
        }
        printf("  dev#%zu (bus=%d): %d OK, %d FAIL\n",di+1,d.bus,ok,fail);
    }

    // Test 2: Rapid TAP x20 (different positions)
    printf("\n[Test 2] Rapid TAP x20 per device\n");
    for(size_t di=0;di<aoa.size();di++){
        auto& d=aoa[di];
        int ok=0,fail=0;
        uint32_t seq=100;
        for(int i=0;i<20;i++){
            TapPayload tap={100+(i*30), 200+(i*50), 800, 1280, 100};
            if(send_cmd(d,CMD_TAP,seq,(uint8_t*)&tap,sizeof(tap))&&recv_ack(d,seq)) ok++;
            else fail++;
            seq++;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        printf("  dev#%zu (bus=%d): %d OK, %d FAIL\n",di+1,d.bus,ok,fail);
    }

    // Test 3: Mixed commands x30
    printf("\n[Test 3] Mixed commands x30 per device\n");
    for(size_t di=0;di<aoa.size();di++){
        auto& d=aoa[di];
        int ok=0,fail=0;
        uint32_t seq=200;
        for(int i=0;i<30;i++){
            bool r=false;
            switch(i%3){
                case 0: r=send_cmd(d,CMD_PING,seq,nullptr,0)&&recv_ack(d,seq); break;
                case 1: {TapPayload t={400,700,800,1280,100};r=send_cmd(d,CMD_TAP,seq,(uint8_t*)&t,sizeof(t))&&recv_ack(d,seq);} break;
                case 2: r=send_cmd(d,CMD_BACK,seq,nullptr,0)&&recv_ack(d,seq); break;
            }
            if(r) ok++; else fail++;
            seq++;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        printf("  dev#%zu (bus=%d): %d OK, %d FAIL\n",di+1,d.bus,ok,fail);
    }

    // Test 4: Sustained connection (PING every 2s for 30s)
    printf("\n[Test 4] Sustained PING every 2s for 30s (all devices)\n");
    auto start=std::chrono::steady_clock::now();
    uint32_t seq=300;
    int total_ok=0,total_fail=0;
    while(true){
        auto elapsed=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-start).count();
        if(elapsed>=30) break;
        for(size_t di=0;di<aoa.size();di++){
            bool r=send_cmd(aoa[di],CMD_PING,seq,nullptr,0)&&recv_ack(aoa[di],seq);
            if(r) total_ok++; else { total_fail++; printf("  [%lds] dev#%zu FAIL\n",elapsed,di+1); }
            seq++;
        }
        printf("  [%lds] all pinged\r",elapsed);fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    printf("\n  Result: %d OK, %d FAIL\n",total_ok,total_fail);

    // Cleanup
    for(auto& d:aoa){libusb_release_interface(d.h,d.iface);libusb_close(d.h);}
    libusb_exit(ctx);

    printf("\n=== Stress Test Complete ===\n");
    return 0;
}
