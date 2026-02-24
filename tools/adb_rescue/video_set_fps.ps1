param(
  [Parameter(Mandatory=$true)][string]$Device,
  [Parameter(Mandatory=$true)][int]$Fps
)

Write-Host "== Set target FPS ($Device) fps=$Fps ==" -ForegroundColor Yellow
& adb -s $Device shell "am broadcast -a com.mirage.capture.ACTION_VIDEO_FPS --ei target_fps $Fps" | Out-Host
