#!/bin/bash
# Enable USB tethering (USBLAN) on X1 via UI automation
# Usage: ./enable_usblan.sh [adb_id]

ADB_ID="${1:-192.168.0.3:5555}"
USBLAN_IP="10.189.194.30"

echo "Checking current USBLAN status..."
if ping -n 1 -w 1000 "$USBLAN_IP" >/dev/null 2>&1; then
    echo "USBLAN already active ($USBLAN_IP)"
    exit 0
fi

echo "USBLAN not active, enabling via UI..."

# Open tethering settings
adb -s "$ADB_ID" shell am start -n com.android.settings/.TetherSettings 2>/dev/null
sleep 2

# Tap USB tethering toggle (coordinates may vary by device)
# X1 1200x2000: USB tethering is typically around y=400-500
adb -s "$ADB_ID" shell input tap 600 450 2>/dev/null
sleep 3

# Verify
if ping -n 1 -w 1000 "$USBLAN_IP" >/dev/null 2>&1; then
    echo "USBLAN enabled successfully ($USBLAN_IP)"
    # Close settings
    adb -s "$ADB_ID" shell input keyevent KEYCODE_HOME 2>/dev/null
    exit 0
else
    echo "USBLAN activation failed - manual intervention required"
    exit 1
fi
