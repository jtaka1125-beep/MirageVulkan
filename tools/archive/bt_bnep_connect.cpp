// bt_bnep_connect.cpp - Direct BNEP/PAN connection via Winsock BT
#include <winsock2.h>
#include <windows.h>
#include <ws2bth.h>
#include <bluetoothapis.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bthprops.lib")

// NAP UUID as 128-bit
static const GUID PAN_NAP_UUID = {0x00001116, 0x0000, 0x1000,
    {0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB}};

bool parse_mac(const char* s, BTH_ADDR* out) {
    unsigned b[6];
    if (sscanf(s,"%2x:%2x:%2x:%2x:%2x:%2x",&b[0],&b[1],&b[2],&b[3],&b[4],&b[5])!=6) return false;
    *out = 0;
    for (int i = 0; i < 6; i++)
        *out = (*out << 8) | b[i];
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: bt_bnep_connect <MAC>\n");
        return 1;
    }

    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    BTH_ADDR addr;
    if (!parse_mac(argv[1], &addr)) {
        printf("Bad MAC\n"); return 1;
    }
    printf("[INFO] Connecting BNEP to %s (addr=0x%012llX)\n", argv[1], addr);

    // Create Bluetooth RFCOMM socket
    SOCKET s = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (s == INVALID_SOCKET) {
        printf("[ERROR] socket failed: %d\n", WSAGetLastError());
        return 1;
    }

    SOCKADDR_BTH sa = {};
    sa.addressFamily = AF_BTH;
    sa.btAddr = addr;
    sa.serviceClassId = PAN_NAP_UUID;
    sa.port = 0;  // Use SDP to find port

    printf("[INFO] Connecting...\n");
    int ret = connect(s, (SOCKADDR*)&sa, sizeof(sa));
    if (ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        printf("[ERROR] connect failed: %d\n", err);
        // Common errors:
        // 10061 = Connection refused (service not available)
        // 10060 = Connection timed out (device not reachable)
        // 10050 = Network is down
        closesocket(s);
        WSACleanup();
        return 1;
    }

    printf("[OK] BNEP connected to %s\n", argv[1]);
    
    // Keep connection alive for a moment
    Sleep(2000);
    
    closesocket(s);
    WSACleanup();
    return 0;
}
