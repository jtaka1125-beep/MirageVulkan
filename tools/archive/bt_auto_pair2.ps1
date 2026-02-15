# Bluetooth auto-pairing v2 - FindAllAsync approach
param(
    [string]$TargetMAC = "",
    [string]$TargetName = "RebotAi"
)

Write-Host "=== Bluetooth Auto-Pair v2 ===" -ForegroundColor Cyan

# Load WinRT
Add-Type -AssemblyName System.Runtime.WindowsRuntime
[Windows.Devices.Bluetooth.BluetoothDevice, Windows.Devices.Bluetooth, ContentType=WindowsRuntime] | Out-Null
[Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType=WindowsRuntime] | Out-Null
[Windows.Devices.Enumeration.DevicePairingKinds, Windows.Devices.Enumeration, ContentType=WindowsRuntime] | Out-Null

$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | 
    Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]

Function Await($WinRtTask, $ResultType) {
    $asTask = $asTaskGeneric.MakeGenericMethod($ResultType)
    $netTask = $asTask.Invoke($null, @($WinRtTask))
    $netTask.Wait(30000) | Out-Null
    $netTask.Result
}

# Step 1: Find unpaired BT devices
Write-Host "[Step 1] Querying unpaired Bluetooth devices..." -ForegroundColor Yellow

$selector = [Windows.Devices.Bluetooth.BluetoothDevice]::GetDeviceSelectorFromPairingState($false)
$devices = Await ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync($selector)) ([Windows.Devices.Enumeration.DeviceInformationCollection])

Write-Host "  Found $($devices.Count) unpaired devices:" -ForegroundColor Gray
foreach ($d in $devices) {
    Write-Host "    $($d.Name) | $($d.Id)" -ForegroundColor Gray
}
Write-Host ""

# Also list paired devices
Write-Host "[Info] Already paired devices:" -ForegroundColor Gray
$pairedSelector = [Windows.Devices.Bluetooth.BluetoothDevice]::GetDeviceSelectorFromPairingState($true)
$pairedDevices = Await ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync($pairedSelector)) ([Windows.Devices.Enumeration.DeviceInformationCollection])
foreach ($d in $pairedDevices) {
    Write-Host "    $($d.Name) | $($d.Id)" -ForegroundColor DarkGreen
}
Write-Host ""

# Step 2: Filter targets
Write-Host "[Step 2] Finding targets..." -ForegroundColor Yellow

$targets = @()
foreach ($d in $devices) {
    $match = $false
    
    if ($TargetMAC -ne "") {
        $macClean = $TargetMAC.Replace(":", "").ToUpper()
        if ($d.Id.ToUpper() -like "*$macClean*") { $match = $true }
    }
    
    if ($d.Name -like "*RebotAi*" -or $d.Name -like "*N-one*" -or $d.Name -like "*Npad*" -or $d.Name -like "*A9*") {
        $match = $true
    }
    
    if ($TargetName -ne "" -and $d.Name -like "*$TargetName*") { $match = $true }
    
    if ($match) {
        $targets += $d
        Write-Host "  -> Target: $($d.Name)" -ForegroundColor Green
    }
}

if ($targets.Count -eq 0) {
    Write-Host "[WARN] No matching targets found" -ForegroundColor Yellow
    Write-Host "  All unpaired devices will be attempted" -ForegroundColor Yellow
    foreach ($d in $devices) {
        if ($d.Name -ne "") { $targets += $d }
    }
}

if ($targets.Count -eq 0) {
    Write-Host "[ERROR] No devices to pair" -ForegroundColor Red
    exit 1
}

# Step 3: Pair
Write-Host ""
Write-Host "[Step 3] Pairing..." -ForegroundColor Yellow

$successCount = 0
foreach ($target in $targets) {
    Write-Host "  Pairing: $($target.Name)..." -NoNewline
    
    try {
        $btDevice = Await ([Windows.Devices.Bluetooth.BluetoothDevice]::FromIdAsync($target.Id)) ([Windows.Devices.Bluetooth.BluetoothDevice])
        
        if ($null -eq $btDevice) {
            Write-Host " FAILED (null)" -ForegroundColor Red
            continue
        }
        
        $di = $btDevice.DeviceInformation
        
        if ($di.Pairing.IsPaired) {
            Write-Host " ALREADY PAIRED" -ForegroundColor Green
            $successCount++
            continue
        }
        
        if (-not $di.Pairing.CanPair) {
            Write-Host " CANNOT PAIR" -ForegroundColor Red
            continue
        }
        
        # Try simple pair first (auto-accept SSP)
        $result = Await ($di.Pairing.PairAsync()) ([Windows.Devices.Enumeration.DevicePairingResult])
        
        if ($result.Status -eq [Windows.Devices.Enumeration.DevicePairingResultStatus]::Paired) {
            Write-Host " OK!" -ForegroundColor Green
            $successCount++
        } elseif ($result.Status -eq [Windows.Devices.Enumeration.DevicePairingResultStatus]::AlreadyPaired) {
            Write-Host " ALREADY PAIRED" -ForegroundColor Green
            $successCount++
        } else {
            Write-Host " $($result.Status)" -ForegroundColor Red
        }
        
    } catch {
        Write-Host " ERROR: $_" -ForegroundColor Red
    }
}

Write-Host ""
Write-Host "=== Result: $successCount paired ===" -ForegroundColor Cyan
exit $(if ($successCount -gt 0) { 0 } else { 1 })
