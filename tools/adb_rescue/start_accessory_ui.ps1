param(
  [Parameter(Mandatory=$true)][string]$Device
)

Write-Host "== Start Mirage Accessory UI ($Device) ==" -ForegroundColor Yellow
& adb -s $Device shell "am start -n com.mirage.accessory/.ui.AccessoryActivity" | Out-Host
Write-Host "done" -ForegroundColor Yellow
