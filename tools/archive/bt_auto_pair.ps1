# Bluetooth auto-pairing via WinRT DeviceInformation API
# Usage: powershell -ExecutionPolicy Bypass -File bt_auto_pair.ps1 [-TargetMAC "XX:XX:XX:XX:XX:XX"]

param(
    [string]$TargetMAC = "",
    [string]$TargetName = "RebotAi",
    [int]$ScanTimeout = 15
)

Write-Host "=== Bluetooth Auto-Pair ===" -ForegroundColor Cyan
Write-Host ""

# Load WinRT assemblies
Add-Type -AssemblyName System.Runtime.WindowsRuntime

# Helper: await async WinRT tasks
$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | 
    Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]

Function Await($WinRtTask, $ResultType) {
    $asTask = $asTaskGeneric.MakeGenericMethod($ResultType)
    $netTask = $asTask.Invoke($null, @($WinRtTask))
    $netTask.Wait(-1) | Out-Null
    $netTask.Result
}

Function AwaitAction($WinRtTask) {
    $netTask = [System.WindowsRuntimeSystemExtensions]::AsTask($WinRtTask)
    $netTask.Wait(-1) | Out-Null
}

# Load Bluetooth types
[Windows.Devices.Bluetooth.BluetoothDevice, Windows.Devices.Bluetooth, ContentType=WindowsRuntime] | Out-Null
[Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType=WindowsRuntime] | Out-Null
[Windows.Devices.Enumeration.DevicePairingKinds, Windows.Devices.Enumeration, ContentType=WindowsRuntime] | Out-Null
[Windows.Devices.Enumeration.DevicePairingResultStatus, Windows.Devices.Enumeration, ContentType=WindowsRuntime] | Out-Null

# Step 1: Scan for Bluetooth devices
Write-Host "[Step 1] Scanning for Bluetooth devices ($ScanTimeout sec)..." -ForegroundColor Yellow

$selector = [Windows.Devices.Bluetooth.BluetoothDevice]::GetDeviceSelectorFromPairingState($false)
$watcher = [Windows.Devices.Enumeration.DeviceInformation]::CreateWatcher($selector)

$foundDevices = [System.Collections.Generic.List[object]]::new()
$scanDone = [System.Threading.ManualResetEvent]::new($false)

$addedHandler = {
    param($sender, $args)
    $dev = $args
    $name = $dev.Name
    $id = $dev.Id
    Write-Host "  Found: $name ($id)" -ForegroundColor Gray
    $foundDevices.Add(@{ Name = $name; Id = $id })
}

Register-ObjectEvent -InputObject $watcher -EventName Added -Action $addedHandler | Out-Null

$watcher.Start()
Start-Sleep -Seconds $ScanTimeout
$watcher.Stop()

Write-Host ""
Write-Host "[Step 1] Found $($foundDevices.Count) unpaired devices" -ForegroundColor Yellow
Write-Host ""

if ($foundDevices.Count -eq 0) {
    Write-Host "[ERROR] No devices found. Make sure Android is in discoverable mode." -ForegroundColor Red
    exit 1
}

# Step 2: Filter target devices
Write-Host "[Step 2] Filtering targets..." -ForegroundColor Yellow

$targets = @()
foreach ($dev in $foundDevices) {
    $match = $false
    
    # Match by MAC address
    if ($TargetMAC -ne "") {
        $macClean = $TargetMAC.Replace(":", "").ToUpper()
        if ($dev.Id -like "*$macClean*") {
            $match = $true
        }
    }
    
    # Match by name
    if ($TargetName -ne "" -and $dev.Name -like "*$TargetName*") {
        $match = $true
    }
    
    # Also match N-one, Npad
    if ($dev.Name -like "*N-one*" -or $dev.Name -like "*Npad*") {
        $match = $true
    }
    
    if ($match) {
        $targets += $dev
        Write-Host "  Target: $($dev.Name)" -ForegroundColor Green
    }
}

if ($targets.Count -eq 0) {
    Write-Host "[WARN] No matching targets. Listing all found devices:" -ForegroundColor Yellow
    foreach ($dev in $foundDevices) {
        Write-Host "  - $($dev.Name) ($($dev.Id))" 
    }
    
    # Try all devices that look like Android
    foreach ($dev in $foundDevices) {
        if ($dev.Name -ne "" -and $dev.Name -ne "Unknown") {
            $targets += $dev
        }
    }
    
    if ($targets.Count -eq 0) {
        Write-Host "[ERROR] No suitable devices to pair" -ForegroundColor Red
        exit 1
    }
}

Write-Host ""

# Step 3: Pair each target
Write-Host "[Step 3] Pairing..." -ForegroundColor Yellow

$successCount = 0
foreach ($target in $targets) {
    Write-Host "  Pairing: $($target.Name)..." -NoNewline
    
    try {
        $btDevice = Await ([Windows.Devices.Bluetooth.BluetoothDevice]::FromIdAsync($target.Id)) ([Windows.Devices.Bluetooth.BluetoothDevice])
        
        if ($null -eq $btDevice) {
            Write-Host " FAILED (device not accessible)" -ForegroundColor Red
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
        
        # Auto-accept pairing (no PIN prompt for SSP)
        $customPairing = $di.Pairing.Custom
        
        $pairingHandler = {
            param($sender, $args)
            Write-Host " [PIN confirm]" -NoNewline -ForegroundColor Cyan
            $args.Accept()
        }
        
        Register-ObjectEvent -InputObject $customPairing -EventName PairingRequested -Action $pairingHandler | Out-Null
        
        $pairingKinds = [Windows.Devices.Enumeration.DevicePairingKinds]::ConfirmOnly -bor 
                        [Windows.Devices.Enumeration.DevicePairingKinds]::ConfirmPinMatch -bor
                        [Windows.Devices.Enumeration.DevicePairingKinds]::DisplayPin
        
        $result = Await ($customPairing.PairAsync($pairingKinds)) ([Windows.Devices.Enumeration.DevicePairingResult])
        
        if ($result.Status -eq [Windows.Devices.Enumeration.DevicePairingResultStatus]::Paired) {
            Write-Host " OK" -ForegroundColor Green
            $successCount++
        } else {
            Write-Host " $($result.Status)" -ForegroundColor Red
        }
        
    } catch {
        Write-Host " ERROR: $_" -ForegroundColor Red
    }
}

Write-Host ""
Write-Host "=== Result: $successCount/$($targets.Count) paired ===" -ForegroundColor Cyan

# Step 4: Verify paired devices
Write-Host ""
Write-Host "[Step 4] Verifying paired devices..." -ForegroundColor Yellow

$pairedSelector = [Windows.Devices.Bluetooth.BluetoothDevice]::GetDeviceSelectorFromPairingState($true)
$pairedDevices = Await ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync($pairedSelector)) ([Windows.Devices.Enumeration.DeviceInformationCollection])

foreach ($pd in $pairedDevices) {
    Write-Host "  Paired: $($pd.Name)" -ForegroundColor Green
}

exit $(if ($successCount -gt 0) { 0 } else { 1 })
