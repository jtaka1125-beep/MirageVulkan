param(
  [Parameter(Mandatory=$true)][string]$Device
)

Write-Host "== Start Mirage Capture UI (MediaProjection permission flow) ($Device) ==" -ForegroundColor Yellow
& adb -s $Device shell "am start -n com.mirage.capture/.ui.CaptureActivity" | Out-Host
Write-Host "NOTE: allow screen-capture prompt on device." -ForegroundColor Cyan
