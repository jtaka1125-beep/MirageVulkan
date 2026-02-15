# bt_pair_pwsh7.ps1 - Custom Pairing with auto-accept (PowerShell 7 + WinRT)
param([string]$TargetMAC = "")

Write-Host "=== BT Auto-Pair (pwsh7) ===" -ForegroundColor Cyan

# Load WinRT
Add-Type -AssemblyName System.Runtime.WindowsRuntime
[Windows.Devices.Bluetooth.BluetoothDevice, Windows.Devices.Bluetooth, ContentType=WindowsRuntime] | Out-Null
[Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType=WindowsRuntime] | Out-Null
[Windows.Devices.Enumeration.DevicePairingKinds, Windows.Devices.Enumeration, ContentType=WindowsRuntime] | Out-Null

# Await helper for pwsh7
$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | 
    Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]

Function Await($WinRtTask, $ResultType) {
    $asTask = $asTaskGeneric.MakeGenericMethod($ResultType)
    $netTask = $asTask.Invoke($null, @($WinRtTask))
    $netTask.Wait(30000) | Out-Null
    $netTask.Result
}

# Find target device
$macLower = $TargetMAC.ToLower()
Write-Host "Target MAC: $macLower"

$selector = [Windows.Devices.Bluetooth.BluetoothDevice]::GetDeviceSelectorFromPairingState($false)
$devices = Await ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync($selector)) ([Windows.Devices.Enumeration.DeviceInformationCollection])

Write-Host "Found $($devices.Count) unpaired devices"
foreach ($d in $devices) { Write-Host "  $($d.Name) | $($d.Id)" -ForegroundColor Gray }

$target = $null
foreach ($d in $devices) {
    if ($d.Id.ToLower().Contains($macLower)) {
        $target = $d
        break
    }
}

if ($null -eq $target) {
    Write-Host "PAIR_RESULT:NOT_FOUND" -ForegroundColor Red
    exit 1
}

Write-Host "Target found: $($target.Name)" -ForegroundColor Green

$btDevice = Await ([Windows.Devices.Bluetooth.BluetoothDevice]::FromIdAsync($target.Id)) ([Windows.Devices.Bluetooth.BluetoothDevice])
$di = $btDevice.DeviceInformation

if ($di.Pairing.IsPaired) {
    Write-Host "PAIR_RESULT:AlreadyPaired" -ForegroundColor Green
    exit 0
}

if (-not $di.Pairing.CanPair) {
    Write-Host "PAIR_RESULT:CannotPair" -ForegroundColor Red
    exit 1
}

# Custom pairing with auto-accept handler
$customPairing = $di.Pairing.Custom

# Register PairingRequested event - pwsh7 supports WinRT events
Register-ObjectEvent -InputObject $customPairing -EventName PairingRequested -Action {
    $kind = $EventArgs.PairingKind
    Write-Host "  PAIRING_KIND: $kind" -ForegroundColor Cyan
    
    if ($kind -eq [Windows.Devices.Enumeration.DevicePairingKinds]::ConfirmPinMatch) {
        Write-Host "  PIN: $($EventArgs.Pin) -> Auto-accepting" -ForegroundColor Yellow
    }
    
    $EventArgs.Accept()
    Write-Host "  Accepted!" -ForegroundColor Green
} | Out-Null

$pairingKinds = [Windows.Devices.Enumeration.DevicePairingKinds]::ConfirmOnly -bor 
                [Windows.Devices.Enumeration.DevicePairingKinds]::ConfirmPinMatch -bor
                [Windows.Devices.Enumeration.DevicePairingKinds]::DisplayPin -bor
                [Windows.Devices.Enumeration.DevicePairingKinds]::ProvidePin

Write-Host "Initiating pairing..." -ForegroundColor Yellow
$result = Await ($customPairing.PairAsync($pairingKinds)) ([Windows.Devices.Enumeration.DevicePairingResult])

Write-Host ""
Write-Host "PAIR_RESULT:$($result.Status)" -ForegroundColor $(if ($result.Status -eq 'Paired') { 'Green' } else { 'Red' })
exit $(if ($result.Status -eq 'Paired') { 0 } else { 1 })
