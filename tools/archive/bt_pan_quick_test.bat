@echo off
chcp 65001 >nul
echo ============================================
echo  BT PAN接続テスト (A9 #956)
echo ============================================

echo [1] BT PAN connect (NAP)...
C:\MirageWork\MirageComplete\build\bt_pan_connect.exe 9C:92:53:DD:20:83

echo.
echo [2] Waiting 5s for BNEP...
timeout /t 5 >nul

echo [3] Adapter status:
powershell -NoProfile -Command "Get-NetAdapter | Where-Object {$_.InterfaceDescription -like '*Bluetooth*'} | Format-Table Name, Status, LinkSpeed"

echo [4] IP addresses:
powershell -NoProfile -Command "Get-NetIPAddress -AddressFamily IPv4 | Where-Object {$_.InterfaceAlias -like '*Bluetooth*'} | Format-Table IPAddress, InterfaceAlias"

echo [5] Android bt-pan IP:
adb -s 192.168.0.6:5555 shell "ip addr show bt-pan 2>/dev/null | grep inet"

echo.
echo [6] Attempting ADB over BT PAN...
for %%i in (192.168.44.1 192.168.44.2 192.168.137.1 192.168.137.2) do (
    echo   Trying %%i:5555 ...
    adb connect %%i:5555 2>nul | findstr /i "connected already" && (
        echo   [OK] %%i:5555
        goto :done
    )
)
echo   No BT PAN ADB connection found

:done
echo.
echo [7] All ADB devices:
adb devices -l
echo.
echo ============================================
echo  Result saved to bt_pan_result.txt
echo ============================================
pause
