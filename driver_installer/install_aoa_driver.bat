@echo off
REM ====================================================
REM  AOA Driver Installer using Zadig
REM  MirageSystem v2.0
REM ====================================================
REM  This script launches Zadig with pre-configured settings
REM  for installing WinUSB driver on AOA devices.
REM ====================================================

echo.
echo ====================================================
echo  MirageSystem - AOA Driver Installer
echo ====================================================
echo.

REM Check admin privileges
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Administrator privileges required!
    echo.
    echo Please right-click this file and select
    echo "Run as administrator"
    echo.
    pause
    exit /b 1
)

cd /d "%~dp0tools"

REM Check if Zadig exists
if not exist "zadig.exe" (
    echo [ERROR] zadig.exe not found in tools folder!
    echo Please download Zadig from https://zadig.akeo.ie/
    pause
    exit /b 1
)

echo [INFO] Starting Zadig...
echo.
echo ====================================================
echo  INSTRUCTIONS:
echo ====================================================
echo  1. In Zadig, go to Options - List All Devices
echo  2. Select the device with VID 18D1 (Google/AOA)
echo  3. Ensure "WinUSB" is selected as target driver
echo  4. Click "Install Driver" or "Replace Driver"
echo  5. Wait for completion
echo ====================================================
echo.

start "" "zadig.exe"

echo [INFO] Zadig launched. Follow the instructions above.
echo.
pause
