# ====================================================
# AOA Driver Installer using Zadig
# MirageSystem v2.0
# ====================================================
# This script auto-elevates to admin and launches Zadig
# ====================================================

param(
    [switch]$Elevated
)

function Test-Admin {
    $currentUser = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    return $currentUser.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Self-elevate if not admin
if (-not (Test-Admin)) {
    if (-not $Elevated) {
        Write-Host "[INFO] Requesting administrator privileges..." -ForegroundColor Yellow
        $scriptPath = $MyInvocation.MyCommand.Path
        Start-Process powershell.exe -Verb RunAs -ArgumentList "-ExecutionPolicy Bypass -File `"$scriptPath`" -Elevated"
        exit
    }
    else {
        Write-Host "[ERROR] Failed to get administrator privileges" -ForegroundColor Red
        pause
        exit 1
    }
}

# Now running as admin
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$toolsDir = Join-Path $scriptDir "tools"
$zadigPath = Join-Path $toolsDir "zadig.exe"

Write-Host ""
Write-Host "====================================================" -ForegroundColor Cyan
Write-Host " MirageSystem - AOA Driver Installer" -ForegroundColor Cyan
Write-Host "====================================================" -ForegroundColor Cyan
Write-Host ""

# Check Zadig
if (-not (Test-Path $zadigPath)) {
    Write-Host "[ERROR] zadig.exe not found!" -ForegroundColor Red
    Write-Host "Path: $zadigPath" -ForegroundColor Red
    pause
    exit 1
}

# Check for AOA device
Write-Host "[INFO] Checking for AOA devices..." -ForegroundColor Yellow
$aoaDevices = Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -like "*VID_18D1&PID_2D0*"
}

if ($aoaDevices) {
    Write-Host "[OK] AOA device found:" -ForegroundColor Green
    $aoaDevices | ForEach-Object {
        Write-Host "     $($_.FriendlyName) [$($_.InstanceId)]" -ForegroundColor White
    }
}
else {
    Write-Host "[WARN] No AOA device detected." -ForegroundColor Yellow
    Write-Host "       Make sure the Android device is connected and in AOA mode." -ForegroundColor Yellow
}

Write-Host ""
Write-Host "====================================================" -ForegroundColor Cyan
Write-Host " INSTRUCTIONS:" -ForegroundColor Cyan
Write-Host "====================================================" -ForegroundColor Cyan
Write-Host " 1. In Zadig: Options -> List All Devices" -ForegroundColor White
Write-Host " 2. Select device with VID 18D1 (Google/AOA)" -ForegroundColor White
Write-Host " 3. Ensure 'WinUSB' is the target driver" -ForegroundColor White
Write-Host " 4. Click 'Install Driver' or 'Replace Driver'" -ForegroundColor White
Write-Host "====================================================" -ForegroundColor Cyan
Write-Host ""

Write-Host "[INFO] Launching Zadig..." -ForegroundColor Yellow
Set-Location $toolsDir
Start-Process -FilePath $zadigPath -Wait

Write-Host ""
Write-Host "[INFO] Zadig closed. Verifying driver..." -ForegroundColor Yellow

# Verify installation
Start-Sleep -Seconds 2
$aoaDevicesAfter = Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -like "*VID_18D1&PID_2D0*"
}

if ($aoaDevicesAfter) {
    $driver = Get-PnpDeviceProperty -InstanceId $aoaDevicesAfter[0].InstanceId -KeyName "DEVPKEY_Device_DriverDesc" -ErrorAction SilentlyContinue
    if ($driver.Data -like "*WinUSB*" -or $driver.Data -like "*libusb*") {
        Write-Host "[SUCCESS] WinUSB driver installed!" -ForegroundColor Green
    }
    else {
        Write-Host "[INFO] Driver: $($driver.Data)" -ForegroundColor Yellow
    }
}

Write-Host ""
pause
