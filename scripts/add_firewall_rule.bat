@echo off
REM =============================================================================
REM MirageTestKit - Add Windows Firewall Rules
REM Run as Administrator
REM =============================================================================

echo.
echo MirageTestKit - Firewall Configuration
echo ======================================
echo.

REM Check for admin rights
net session >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] This script requires Administrator privileges.
    echo         Right-click and select "Run as administrator"
    pause
    exit /b 1
)

echo Adding firewall rules for MirageTestKit...
echo.

REM Video ports (UDP 60000-60010)
echo [1/4] Video streaming ports (UDP 60000-60010)...
netsh advfirewall firewall add rule name="MirageTestKit Video (UDP IN)" dir=in action=allow protocol=UDP localport=60000-60010 >nul
if %ERRORLEVEL% EQU 0 (echo       OK) else (echo       FAILED)

REM Command ports (TCP 50000-50010)
echo [2/4] Command ports (TCP 50000-50010)...
netsh advfirewall firewall add rule name="MirageTestKit Command (TCP IN)" dir=in action=allow protocol=TCP localport=50000-50010 >nul
if %ERRORLEVEL% EQU 0 (echo       OK) else (echo       FAILED)

REM ADB ports
echo [3/4] ADB ports (TCP 5555, 5037)...
netsh advfirewall firewall add rule name="ADB WiFi (TCP IN)" dir=in action=allow protocol=TCP localport=5555 >nul
netsh advfirewall firewall add rule name="ADB Server (TCP IN)" dir=in action=allow protocol=TCP localport=5037 >nul
if %ERRORLEVEL% EQU 0 (echo       OK) else (echo       FAILED)

REM IPC port
echo [4/4] IPC port (TCP 50100)...
netsh advfirewall firewall add rule name="MirageTestKit IPC (TCP IN)" dir=in action=allow protocol=TCP localport=50100 >nul
if %ERRORLEVEL% EQU 0 (echo       OK) else (echo       FAILED)

echo.
echo ======================================
echo Firewall rules added successfully!
echo ======================================
echo.
pause
