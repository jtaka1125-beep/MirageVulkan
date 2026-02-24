param(
  [Parameter(Mandatory=$true)][string]$Device
)

function Run($cmd) {
  Write-Host "> $cmd" -ForegroundColor Cyan
  & adb -s $Device shell $cmd 2>&1
}

Write-Host "== Mirage diag for $Device ==" -ForegroundColor Yellow

Write-Host "\n[1] Packages" -ForegroundColor Green
Run "pm list packages | grep -E 'com\\.mirage\\.' || true"

Write-Host "\n[2] Versions" -ForegroundColor Green
Run "for p in com.mirage.accessory com.mirage.capture com.mirage.android; do echo \"-- $p\"; dumpsys package $p 2>/dev/null | grep -E 'versionName|versionCode|firstInstallTime|lastUpdateTime' | head -n 10; done"

Write-Host "\n[3] Processes" -ForegroundColor Green
Run "ps -A | grep mirage | head -n 20 || true"

Write-Host "\n[4] MediaProjection" -ForegroundColor Green
Run "dumpsys media_projection | head -n 40"

Write-Host "\n[5] Recent logcat (mirage only)" -ForegroundColor Green
& adb -s $Device logcat -d -v brief | findstr /I "mirage" | Select-Object -Last 80

Write-Host "\n== done ==" -ForegroundColor Yellow
