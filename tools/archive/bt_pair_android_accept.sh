#!/bin/bash
# bt_pair_android_accept.sh
# Android側のBluetoothペアリングダイアログを自動承認
# PC側のPairAsyncと並行して実行する

DEVICE=$1
if [ -z "$DEVICE" ]; then
    echo "Usage: $0 <device_serial>"
    exit 1
fi

echo "[Android] Watching for Bluetooth pairing dialog on $DEVICE..."

for i in $(seq 1 30); do
    # Check if pairing dialog is showing
    FOCUS=$(adb -s "$DEVICE" shell dumpsys window 2>/dev/null | grep "currentFocus" | head -1)
    
    if echo "$FOCUS" | grep -qi "bluetooth.*pair\|BluetoothPair\|RequestPermission\|PhonePermission"; then
        echo "[Android] Pairing dialog detected!"
        
        # Try to find and click the "Pair" / "ペア設定する" button via UI dump
        UIDUMP=$(adb -s "$DEVICE" shell uiautomator dump /dev/tty 2>/dev/null)
        
        # Look for pair button coordinates
        # Japanese: "ペア設定する" or "ペアリング"  English: "Pair" or "OK"
        PAIR_BOUNDS=$(echo "$UIDUMP" | grep -oP 'text="(ペア設定する|PAIR|Pair|OK|ペアリング)"[^>]*bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"' | head -1 | grep -oP '\[\d+,\d+\]' )
        
        if [ -n "$PAIR_BOUNDS" ]; then
            # Extract center of button
            X1=$(echo "$PAIR_BOUNDS" | head -1 | grep -oP '\d+' | head -1)
            Y1=$(echo "$PAIR_BOUNDS" | head -1 | grep -oP '\d+' | tail -1)
            X2=$(echo "$PAIR_BOUNDS" | tail -1 | grep -oP '\d+' | head -1)
            Y2=$(echo "$PAIR_BOUNDS" | tail -1 | grep -oP '\d+' | tail -1)
            CX=$(( (X1 + X2) / 2 ))
            CY=$(( (Y1 + Y2) / 2 ))
            echo "[Android] Tapping pair button at ($CX, $CY)"
            adb -s "$DEVICE" shell input tap $CX $CY
            echo "[Android] Pair accepted!"
            exit 0
        else
            # Fallback: try clicking common locations or use keyevent
            echo "[Android] Button not found in UI dump, trying ENTER key"
            adb -s "$DEVICE" shell input keyevent KEYCODE_ENTER
            sleep 1
            # Also try tab + enter
            adb -s "$DEVICE" shell input keyevent KEYCODE_TAB
            adb -s "$DEVICE" shell input keyevent KEYCODE_ENTER
            echo "[Android] Sent ENTER"
            exit 0
        fi
    fi
    
    sleep 1
done

echo "[Android] No pairing dialog appeared within 30s"
exit 1
