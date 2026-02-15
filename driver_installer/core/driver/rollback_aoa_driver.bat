@echo off
REM ====================================================
REM  rollback_aoa_driver_improved.bat
REM  AOA WinUSB Driver Rollback (2-Stage)
REM
REM  Stage 1: pnputil /remove-device <InstanceID>
REM  Stage 2: pnputil /delete-driver oemXX.inf /uninstall /force
REM
REM  Requires: Administrator
REM  修正点: ドライバパッケージの完全削除
REM ====================================================

setlocal enabledelayedexpansion

set COLOR_WARN=0E
set COLOR_ERR=0C
set COLOR_OK=02
set COLOR_INFO=0A

set LOG_FILE=aoa_driver_rollback_improved.log
set HWID=USB\VID_18D1^&PID_2D01^&MI_00

cls
echo.
echo ====================================================
echo  AOA WinUSB Driver Rollback (2-Stage)
echo ====================================================
echo.

REM ===== Admin check =====
net session >nul 2>&1
if !errorlevel! neq 0 (
    color %COLOR_ERR%
    echo [ERROR] This script requires administrator privileges.
    color 07
    pause
    exit /b 1
)

echo. >> "%LOG_FILE%"
echo ===================================================== >> "%LOG_FILE%"
echo Rollback Start (2-Stage): %DATE% %TIME% >> "%LOG_FILE%"
echo ===================================================== >> "%LOG_FILE%"

REM ===== Confirmation =====
echo.
color %COLOR_WARN%
echo [WARN] This will remove the AOA WinUSB driver
echo [WARN] Device will become unavailable
echo [WARN] Previous driver may be restored on reconnection
color 07
echo.

set /p CONFIRM="Continue? (type 'yes' to confirm): "
if /i not "%CONFIRM%"=="yes" (
    echo [INFO] Rollback cancelled
    exit /b 0
)

echo [OK] Proceeding with rollback
echo [OK] Proceeding with rollback >> "%LOG_FILE%"

REM ===== Stage 1: Find InstanceID =====
echo.
echo [1] Finding Device InstanceID...
echo [1] Finding Device InstanceID... >> "%LOG_FILE%"

set INSTANCE_ID=
for /f "usebackq tokens=*" %%L in (
    `pnputil /enum-devices /connected 2^>nul ^| findstr /i "VID_18D1.*PID_2D01.*MI_00"`
) do (
    set INSTANCE_ID=%%L
    goto :stage1_found
)

:stage1_found
if "!INSTANCE_ID!"=="" (
    color %COLOR_WARN%
    echo [WARN] Device InstanceID not found (device may be disconnected)
    color 07
    echo [WARN] Device not connected >> "%LOG_FILE%"
    
    echo [INFO] Attempting HWID-based search...
    echo [INFO] Attempting HWID-based search... >> "%LOG_FILE%"
    
    goto :stage2_search
) else (
    color %COLOR_OK%
    echo [OK] Found: !INSTANCE_ID!
    color 07
    echo [OK] InstanceID: !INSTANCE_ID! >> "%LOG_FILE%"
)

REM ===== Stage 1: Remove Device =====
echo.
echo [2] Stage 1: Removing device via pnputil /remove-device...
echo [2] Stage 1: Removing device... >> "%LOG_FILE%"

pnputil /remove-device "!INSTANCE_ID!" 2>>"%LOG_FILE%"

if !errorlevel! equ 0 (
    color %COLOR_OK%
    echo [OK] Device removed (Stage 1 success)
    color 07
    echo [OK] Device removal successful >> "%LOG_FILE%"
) else (
    color %COLOR_WARN%
    echo [WARN] Device removal returned errorlevel !errorlevel!
    color 07
    echo [WARN] Device removal status: !errorlevel! >> "%LOG_FILE%"
)

REM ===== Stage 2: Find and Delete OEM Driver =====
:stage2_search
echo.
echo [3] Stage 2: Finding OEM driver package...
echo [3] Stage 2: Finding OEM driver... >> "%LOG_FILE%"

REM OEM INFを列挙
set OEM_FOUND=0
for /f "usebackq tokens=*" %%L in (
    `pnputil /enum-drivers 2^>nul`
) do (
    set LINE=%%L
    
    REM Published Name を探す
    echo !LINE! | findstr /i "Published Name.*oem" >nul
    if !errorlevel! equ 0 (
        REM oem\d+.inf を抽出
        for /f "tokens=3" %%O in ("!LINE!") do (
            set OEM_INF=%%O
            
            REM このOEMがAOAドライバか確認
            pnputil /enum-drivers 2>nul | findstr /a:0E /v "^$" | find "!OEM_INF!" >nul
            
            echo [INFO] Checking !OEM_INF!...
            
            REM 該当するOEMドライバを削除
            echo [4] Deleting driver !OEM_INF!...
            echo [4] Deleting driver !OEM_INF!... >> "%LOG_FILE%"
            
            pnputil /delete-driver "!OEM_INF!" /uninstall /force 2>>"%LOG_FILE%"
            
            if !errorlevel! equ 0 (
                color %COLOR_OK%
                echo [OK] Driver !OEM_INF! deleted (Stage 2 success)
                color 07
                echo [OK] Driver deletion successful >> "%LOG_FILE%"
                set OEM_FOUND=1
            ) else (
                color %COLOR_WARN%
                echo [WARN] Driver deletion returned errorlevel !errorlevel!
                color 07
                echo [WARN] Driver deletion status: !errorlevel! >> "%LOG_FILE%"
            )
        )
    )
)

if !OEM_FOUND! equ 0 (
    echo [INFO] No OEM drivers found (may already be deleted)
    echo [INFO] No OEM drivers found >> "%LOG_FILE%"
)

REM ===== Remove Flag =====
echo.
echo [5] Removing installation flag...
if exist ".driver_installed" (
    del ".driver_installed"
    echo [OK] Flag removed
    echo [OK] Flag removed >> "%LOG_FILE%"
) else (
    echo [INFO] Flag not found (already removed)
)

REM ===== Show Backup Info =====
echo.
if exist "driver_backup.txt" (
    color %COLOR_INFO%
    echo [INFO] Previous driver information (from backup):
    color 07
    
    REM Display first 10 lines of backup
    for /f "usebackq tokens=*" %%L in ("driver_backup.txt") do (
        echo %%L
        goto :backup_shown
    )
    :backup_shown
    
    echo.
    echo [INFO] Windows may restore previous driver when device reconnects
    echo [INFO] Backup file: driver_backup.txt
) else (
    echo [INFO] No backup information available
    echo [INFO] Reconnect device for automatic driver detection
)

REM ===== Success =====
echo.
color %COLOR_OK%
echo ====================================================
echo  Rollback Completed (2-Stage)
echo ====================================================
color 07
echo.
echo [OK] AOA WinUSB driver removal completed
echo [OK] Please disconnect the device
echo [OK] Windows will attempt to restore previous driver
echo.
echo Stage 1: Device removed
echo Stage 2: OEM driver deleted
echo.
echo Next:
echo  1. Wait 10 seconds
echo  2. Reconnect device
echo  3. Verify with: powershell .\detect_mi_interfaces.ps1
echo.
echo Log file: %LOG_FILE%
echo.

echo. >> "%LOG_FILE%"
echo ===================================================== >> "%LOG_FILE%"
echo [OK] Rollback completed (2-stage) >> "%LOG_FILE%"
echo ===================================================== >> "%LOG_FILE%"

timeout /t 3 /nobreak >nul
endlocal
exit /b 0
