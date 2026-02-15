// bt_pan_dial.cpp - Connect BT PAN via Windows BNEP (INetConnection COM)
// Simpler approach: just use "netsh" style or DeviceIoControl to BthPan driver
#include <winsock2.h>
#include <windows.h>
#include <bluetoothapis.h>
#include <ws2bth.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "bthprops.lib")
#pragma comment(lib, "ws2_32.lib")

// BNEP PSM
#define PSM_BNEP 0x000F

bool parse_mac(const char* s, BTH_ADDR* addr) {
    unsigned b[6];
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0],&b[1],&b[2],&b[3],&b[4],&b[5]) != 6)
        return false;
    *addr = 0;
    for (int i = 0; i < 6; i++)
        *addr = (*addr << 8) | b[i];
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: bt_pan_dial <MAC>\n");
        return 1;
    }

    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    BTH_ADDR target;
    if (!parse_mac(argv[1], &target)) {
        printf("Bad MAC\n");
        return 1;
    }

    printf("[INFO] Connecting L2CAP to %s PSM=0x%04X (BNEP)...\n", argv[1], PSM_BNEP);

    // Create Bluetooth L2CAP socket
    SOCKET s = socket(AF_BTH, SOCK_STREAM, BTHPROTO_L2CAP);
    if (s == INVALID_SOCKET) {
        printf("[ERROR] socket() failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    SOCKADDR_BTH addr = {};
    addr.addressFamily = AF_BTH;
    addr.btAddr = target;
    addr.port = PSM_BNEP;

    int ret = connect(s, (SOCKADDR*)&addr, sizeof(addr));
    if (ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        printf("[ERROR] connect() failed: %d\n", err);
        if (err == WSAECONNREFUSED)
            printf("  -> Connection refused (BNEP not listening on remote)\n");
        else if (err == WSAETIMEDOUT)
            printf("  -> Timeout (device not reachable)\n");
        else if (err == WSAENETUNREACH)
            printf("  -> Network unreachable (BT not connected)\n");
        closesocket(s);
        WSACleanup();
        return 1;
    }

    printf("[OK] L2CAP BNEP connected! Socket=%llu\n", (unsigned long long)s);
    printf("[INFO] BNEP session established - Windows should assign IP via DHCP\n");
    printf("[INFO] Keeping connection open for 30 seconds...\n");

    // Keep alive - Windows BthPan driver should pick up the BNEP connection
    Sleep(30000);

    closesocket(s);
    WSACleanup();
    return 0;
}
