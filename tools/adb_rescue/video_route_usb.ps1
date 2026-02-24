param(
  [Parameter(Mandatory=$true)][string]$Device
)

# route_mode: 0=USB, 1=Wi-Fi(UDP)
Write-Host "== Switch video route -> USB ($Device) ==" -ForegroundColor Yellow
& adb -s $Device shell "am broadcast -a com.mirage.capture.ACTION_VIDEO_ROUTE --ei route_mode 0 --es host '' --ei port 0" | Out-Host
