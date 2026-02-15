@echo off
setlocal EnableDelayedExpansion
REM ============================================
REM  AOA Mode Switch + WinUSB Auto-Install
REM ============================================

set PATH=C:\msys64\mingw64\bin;%PATH%

echo ============================================
echo  MirageSystem - AOA WinUSB Setup
echo ============================================
echo.

echo [1/3] Switching device to AOA Accessory mode...
C:\MirageWork\MirageComplete\tests\aoa_mira_tap_test.exe 2>nul

echo.
echo [2/3] Waiting for AOA device in Windows...
set FOUND=0
for /L %%i in (1,1,15) do (
    if !FOUND!==0 (
        timeout /t 1 /nobreak >nul
        powershell -NoProfile -Command "if (Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like '*VID_18D1*PID_2D0*' }) { exit 0 } else { exit 1 }" >nul 2>&1
        if !errorlevel!==0 (
            echo   AOA device detected!
            set FOUND=1
        ) else (
            echo   Waiting... ^(%%i/15^)
        )
    )
)

if !FOUND!==0 (
    echo [FAIL] AOA device not found after 15 seconds
    pause
    exit /b 1
)

echo.
echo [3/3] Installing WinUSB driver ^(UAC prompt will appear^)...
powershell -NoProfile -ExecutionPolicy Bypass -File "C:\MirageWork\MirageComplete\scripts\install_aoa_winusb.ps1" -ResultFile "C:\MirageWork\MirageComplete\scripts\install_result.json"

echo.
echo Setup complete.
pause
