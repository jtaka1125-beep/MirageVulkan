@echo off
REM MirageSystem AOA Test Workflow
REM Automates: ADB driver -> WiFi ADB -> WinUSB driver -> AOA test
setlocal

set ADB=adb
set A9_SERIAL=A9250700956
set A9_IP=192.168.0.6
set WDI=C:\MirageWork\MirageComplete\driver_installer\tools\wdi\wdi-simple.exe
set TESTS=C:\MirageWork\MirageComplete\tests

echo === Step 1: Ensure WiFi ADB ===
%ADB% -s %A9_SERIAL% tcpip 5555
if errorlevel 1 (
    echo [!] USB ADB not available, trying WiFi...
    %ADB% connect %A9_IP%:5555
)
timeout /t 2 /nobreak > nul
%ADB% connect %A9_IP%:5555
echo.

echo === Step 2: Install WinUSB for 0E8D:201C ===
%ADB% kill-server
timeout /t 1 /nobreak > nul
%WDI% --name "A9 WinUSB" --vid 0x0E8D --pid 0x201C --type 0 --dest C:\MirageWork\a9_winusb_auto
echo.

echo === Step 3: Run AOA test ===
set PATH=C:\msys64\mingw64\bin;%PATH%
%TESTS%\aoa_detect_test2.exe
echo.

echo === Step 4: Restore ADB driver ===
echo [*] Removing WinUSB INF and rescanning...
REM Find and delete the newly created WinUSB OEM inf
for /f "tokens=1,2 delims=:" %%a in ('pnputil /enum-drivers ^| findstr /i "A9 WinUSB"') do (
    echo Found: %%a %%b
)
echo.
echo [!] To restore ADB: pnputil /restart-device then scan
echo === Done ===
