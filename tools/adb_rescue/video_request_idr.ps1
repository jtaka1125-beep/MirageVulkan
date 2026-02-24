param(
  [Parameter(Mandatory=$true)][string]$Device
)

Write-Host "== Request IDR (keyframe) ($Device) ==" -ForegroundColor Yellow
& adb -s $Device shell "am broadcast -a com.mirage.capture.ACTION_VIDEO_IDR" | Out-Host
