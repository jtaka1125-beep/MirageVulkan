param(
  [Parameter(Mandatory=$true)][string]$Device,
  [string]$Host = "192.168.0.2",
  [int]$Port = 50000
)

# route_mode: 0=USB, 1=Wi-Fi(UDP)
Write-Host "== Switch video route -> Wi-Fi(UDP) ($Device) host=$Host port=$Port ==" -ForegroundColor Yellow
& adb -s $Device shell "am broadcast -a com.mirage.capture.ACTION_VIDEO_ROUTE --ei route_mode 1 --es host $Host --ei port $Port" | Out-Host
