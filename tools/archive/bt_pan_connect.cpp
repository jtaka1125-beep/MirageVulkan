// bt_pan_connect.cpp - BT PAN connect via BluetoothSetServiceState
#include <winsock2.h>
#include <windows.h>
#include <bluetoothapis.h>
#include <ws2bth.h>
#include <cstdio>

#pragma comment(lib, "bthprops.lib")
#pragma comment(lib, "ws2_32.lib")

// {00001116-0000-1000-8000-00805F9B34FB} PAN NAP
static GUID g_nap = {0x00001116, 0x0000, 0x1000, {0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB}};
// {00001115-0000-1000-8000-00805F9B34FB} PANU
static GUID g_panu = {0x00001115, 0x0000, 0x1000, {0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB}};

bool parse_mac(const char* s, BLUETOOTH_ADDRESS* a) {
    unsigned b[6];
    if (sscanf(s,"%2x:%2x:%2x:%2x:%2x:%2x",&b[0],&b[1],&b[2],&b[3],&b[4],&b[5])!=6) return false;
    for (int i=0;i<6;i++) a->rgBytes[5-i]=(BYTE)b[i];
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: bt_pan_connect <MAC>\n"); return 1;
    }

    BLUETOOTH_FIND_RADIO_PARAMS rfp={sizeof(rfp)};
    HANDLE hRadio=NULL;
    auto hf=BluetoothFindFirstRadio(&rfp,&hRadio);
    if (!hf) { printf("No radio\n"); return 1; }
    BluetoothFindRadioClose(hf);

    BLUETOOTH_ADDRESS addr={};
    if (!parse_mac(argv[1], &addr)) { printf("Bad MAC\n"); return 1; }

    // Enumerate all paired devices and find matching one
    BLUETOOTH_DEVICE_SEARCH_PARAMS sp={};
    sp.dwSize = sizeof(sp);
    sp.fReturnAuthenticated = TRUE;
    sp.fReturnRemembered = TRUE;
    sp.fReturnConnected = TRUE;
    sp.fReturnUnknown = FALSE;
    sp.fIssueInquiry = FALSE;
    sp.cTimeoutMultiplier = 0;
    sp.hRadio = hRadio;

    BLUETOOTH_DEVICE_INFO di={};
    di.dwSize = sizeof(di);

    HBLUETOOTH_DEVICE_FIND hDev = BluetoothFindFirstDevice(&sp, &di);
    bool found = false;
    if (hDev) {
        do {
            // Compare addresses
            if (memcmp(&di.Address, &addr, sizeof(BLUETOOTH_ADDRESS)) == 0) {
                found = true;
                break;
            }
        } while (BluetoothFindNextDevice(hDev, &di));
        BluetoothFindDeviceClose(hDev);
    }

    if (!found) {
        printf("[ERROR] Device %s not found in paired devices\n", argv[1]);
        CloseHandle(hRadio);
        return 1;
    }

    printf("[INFO] Found: %ls (%s) auth=%d conn=%d cls=0x%08lx\n",
           di.szName, argv[1], di.fAuthenticated, di.fConnected, di.ulClassofDevice);
    printf("[INFO] dwSize=%lu\n", di.dwSize);

    // Try NAP
    printf("[INFO] Trying NAP...\n");
    DWORD r = BluetoothSetServiceState(hRadio, &di, &g_nap, BLUETOOTH_SERVICE_ENABLE);
    printf("  NAP result: %lu\n", r);

    if (r != 0) {
        printf("[INFO] Trying PANU...\n");
        r = BluetoothSetServiceState(hRadio, &di, &g_panu, BLUETOOTH_SERVICE_ENABLE);
        printf("  PANU result: %lu\n", r);
    }

    // Also try disable then enable
    if (r != 0) {
        printf("[INFO] Trying disable+enable NAP...\n");
        BluetoothSetServiceState(hRadio, &di, &g_nap, BLUETOOTH_SERVICE_DISABLE);
        Sleep(1000);
        r = BluetoothSetServiceState(hRadio, &di, &g_nap, BLUETOOTH_SERVICE_ENABLE);
        printf("  NAP re-enable result: %lu\n", r);
    }

    CloseHandle(hRadio);
    return r == 0 ? 0 : 1;
}
