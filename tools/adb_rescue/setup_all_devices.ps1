param(
  [ValidateSet('release','debug')][string]$Variant = 'release',
  [int]$DelaySec = 15
)

$ErrorActionPreference='Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$one = Join-Path $scriptDir 'setup_one_device.ps1'

# Known mapping (USB serial -> Wi-Fi IP)
$devices = @(
  @{ name='X1';     usb='93020523431940'; wifi='192.168.0.3' },
  @{ name='A9_956'; usb='A9250700956';    wifi='192.168.0.6' },
  @{ name='A9_479'; usb='A9250700479';    wifi='192.168.0.8' }
)

foreach($d in $devices){
  Write-Host ("\n===== Setup " + $d.name + " =====") -ForegroundColor Yellow
  & $one -Name $d.name -UsbSerial $d.usb -WifiIp $d.wifi -Variant $Variant
  Write-Host ("Delay " + $DelaySec + "s...") -ForegroundColor Cyan
  Start-Sleep -Seconds $DelaySec
}

Write-Host "\nALL DONE. After permission is granted on each device, switch MirageVulkan to AOA run mode." -ForegroundColor Green
