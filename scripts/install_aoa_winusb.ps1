<#
.SYNOPSIS
  MirageSystem AOA WinUSB Driver Auto-Installer
  UAC昇格して wdi-simple.exe でWinUSBを自動インストール
  
.DESCRIPTION
  1. AOAデバイス(VID_18D1, PID_2D01/2D00)の存在を確認
  2. UACで管理者昇格
  3. wdi-simple.exeでWinUSBドライバをサイレントインストール
  4. 結果をファイルに書き出し（呼び出し元が確認可能）
#>

param(
    [switch]$Elevated,
    [string]$ResultFile = "$PSScriptRoot\install_result.json"
)

$ErrorActionPreference = "Stop"
$WdiExe = "$PSScriptRoot\..\..\driver_installer\tools\wdi\wdi-simple.exe"
$WdiExeResolved = (Resolve-Path $WdiExe -ErrorAction SilentlyContinue).Path

# Fallback paths
if (-not $WdiExeResolved) {
    $alt = "C:\MirageWork\MirageComplete\driver_installer\tools\wdi\wdi-simple.exe"
    if (Test-Path $alt) { $WdiExeResolved = $alt }
}

function Write-Result {
    param([bool]$Success, [string]$Message, [string]$Details = "")
    $result = @{
        success = $Success
        message = $Message
        details = $Details
        timestamp = (Get-Date -Format "yyyy-MM-dd HH:mm:ss")
    } | ConvertTo-Json
    $result | Out-File -FilePath $ResultFile -Encoding UTF8
    Write-Host $result
}

# ============================================
# 管理者昇格チェック
# ============================================
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin -and -not $Elevated) {
    Write-Host "[*] Requesting administrator privileges (UAC)..."
    
    $argList = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Elevated -ResultFile `"$ResultFile`""
    
    try {
        $proc = Start-Process powershell.exe -Verb RunAs -ArgumentList $argList -PassThru -Wait
        
        # 結果ファイル読み取り
        if (Test-Path $ResultFile) {
            $res = Get-Content $ResultFile -Raw | ConvertFrom-Json
            if ($res.success) {
                Write-Host "[OK] $($res.message)" -ForegroundColor Green
                exit 0
            } else {
                Write-Host "[FAIL] $($res.message)" -ForegroundColor Red
                exit 1
            }
        } else {
            Write-Host "[FAIL] No result file - UAC may have been denied" -ForegroundColor Red
            exit 1
        }
    } catch {
        Write-Host "[FAIL] UAC elevation failed: $_" -ForegroundColor Red
        exit 1
    }
}

# ============================================
# ここから管理者権限で実行
# ============================================
Write-Host "============================================"
Write-Host "  MirageSystem AOA WinUSB Auto-Installer"
Write-Host "  Running as Administrator"
Write-Host "============================================"

# Step 1: AOAデバイス確認
Write-Host "`n[1/3] Checking for AOA devices..."
$aoaDevices = Get-PnpDevice -PresentOnly | Where-Object { 
    $_.InstanceId -like "*VID_18D1*PID_2D0*" 
}

if (-not $aoaDevices) {
    Write-Result -Success $false -Message "No AOA device found (VID_18D1, PID_2D0x)"
    exit 1
}

foreach ($dev in $aoaDevices) {
    Write-Host "  Found: $($dev.InstanceId) [$($dev.FriendlyName)] Status=$($dev.Status)"
}

# Step 2: 既にWinUSBが割り当てられてるか確認
$needInstall = $false
foreach ($dev in $aoaDevices) {
    $svc = (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName "DEVPKEY_Device_Service" -ErrorAction SilentlyContinue).Data
    if ($svc -eq "WinUSB") {
        Write-Host "  $($dev.InstanceId): Already has WinUSB" -ForegroundColor Green
    } else {
        Write-Host "  $($dev.InstanceId): Service='$svc' - needs WinUSB" -ForegroundColor Yellow
        $needInstall = $true
    }
}

if (-not $needInstall) {
    Write-Result -Success $true -Message "All AOA devices already have WinUSB driver"
    exit 0
}

# Step 3: wdi-simple.exe でインストール
Write-Host "`n[2/3] Installing WinUSB driver via wdi-simple..."

if (-not $WdiExeResolved -or -not (Test-Path $WdiExeResolved)) {
    Write-Result -Success $false -Message "wdi-simple.exe not found" -Details "Searched: $WdiExe"
    exit 1
}

# PID_2D01 (AOA+ADB) と PID_2D00 (AOA only) の両方にインストール
$pids = @("0x2D01", "0x2D00")
$installed = 0
$errors = @()

foreach ($pid in $pids) {
    # このPIDのデバイスが存在するか確認
    $pidHex = $pid -replace "0x", ""
    $exists = $aoaDevices | Where-Object { $_.InstanceId -like "*PID_$pidHex*" }
    if (-not $exists) { continue }
    
    Write-Host "  Installing for VID=0x18D1 PID=$pid..."
    
    # wdi-simpleを一時ディレクトリで実行（ファイルロック回避）
    $tempDir = "$env:TEMP\mirage_wdi_$pidHex"
    New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
    
    $proc = Start-Process -FilePath $WdiExeResolved `
        -ArgumentList "--vid 0x18D1 --pid $pid --type 0 --name `"Android Accessory (AOA)`" --silent" `
        -WorkingDirectory $tempDir `
        -NoNewWindow -Wait -PassThru
    
    if ($proc.ExitCode -eq 0) {
        Write-Host "  [OK] PID=$pid installed successfully" -ForegroundColor Green
        $installed++
    } else {
        $msg = "PID=$pid failed (exit=$($proc.ExitCode))"
        Write-Host "  [FAIL] $msg" -ForegroundColor Red
        $errors += $msg
    }
    
    # 一時ディレクトリ削除
    Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}

# Step 4: 検証
Write-Host "`n[3/3] Verifying installation..."
Start-Sleep -Seconds 2

$allOk = $true
$aoaDevices2 = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like "*VID_18D1*PID_2D0*" }
foreach ($dev in $aoaDevices2) {
    $svc = (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName "DEVPKEY_Device_Service" -ErrorAction SilentlyContinue).Data
    $status = $dev.Status
    Write-Host "  $($dev.InstanceId): Service=$svc Status=$status"
    if ($svc -ne "WinUSB") { $allOk = $false }
}

if ($allOk -and $installed -gt 0) {
    Write-Result -Success $true -Message "WinUSB installed for $installed device(s)" -Details "All AOA devices verified"
} elseif ($installed -gt 0) {
    Write-Result -Success $true -Message "WinUSB installed for $installed device(s)" -Details "Some devices may need re-plug. Errors: $($errors -join '; ')"
} else {
    Write-Result -Success $false -Message "Installation failed" -Details ($errors -join '; ')
}
