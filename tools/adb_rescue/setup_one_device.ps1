param(
  [Parameter(Mandatory=$true)][string]$UsbSerial,
  [Parameter(Mandatory=$true)][string]$WifiIp,
  [int]$WifiPort = 5555,
  [ValidateSet('release','debug')][string]$Variant = 'release',
  # Optional friendly name (X1 / A9_956 / A9_479)
  [string]$Name = ""
)

$ErrorActionPreference = 'Stop'

function Step($msg){ Write-Host ("== " + $msg) -ForegroundColor Yellow }
function Ok($msg){ Write-Host ("OK: " + $msg) -ForegroundColor Green }
function Warn($msg){ Write-Host ("WARN: " + $msg) -ForegroundColor Cyan }

$adb = "adb"
$wifi = "$WifiIp`:$WifiPort"

# ------------------------------------------------------------
# Safety: ensure only one USB device (prevents wrong targeting)
# ------------------------------------------------------------
Step "Check USB devices"
$usbList = & $adb devices -l | Select-String -Pattern " usb" | ForEach-Object { $_.Line }
if($usbList.Count -gt 1){
  throw "Multiple USB devices detected. Unplug others and retry. Found: $($usbList -join '; ')"
}

Step "Verify USB serial is visible"
$all = & $adb devices -l
if(-not ($all -match [regex]::Escape($UsbSerial))){
  throw "USB serial not found in adb devices -l: $UsbSerial"
}
Ok "USB serial detected: $UsbSerial"

# ------------------------------------------------------------
# APK paths
# ------------------------------------------------------------
$root = "C:\MirageWork\MirageVulkan\android"
$accApk = if($Variant -eq 'release'){"$root\accessory\build\outputs\apk\release\accessory-release.apk"} else {"$root\accessory\build\outputs\apk\debug\accessory-debug.apk"}
$capApk = if($Variant -eq 'release'){"$root\capture\build\outputs\apk\release\capture-release.apk"} else {"$root\capture\build\outputs\apk\debug\capture-debug.apk"}

if(!(Test-Path $accApk)){ throw "Missing accessory apk: $accApk" }
if(!(Test-Path $capApk)){ throw "Missing capture apk: $capApk" }

# ------------------------------------------------------------
# Install/Update APKs
# ------------------------------------------------------------
Step "Install/Update APKs (USB ADB)"
& $adb -s $UsbSerial install -r $accApk | Out-Host
& $adb -s $UsbSerial install -r $capApk | Out-Host
Ok "APKs installed"

# ------------------------------------------------------------
# Enable + connect Wi-Fi ADB
# ------------------------------------------------------------
Step "Enable Wi-Fi ADB (tcpip)"
& $adb -s $UsbSerial tcpip $WifiPort | Out-Host
Start-Sleep -Seconds 1

Step "Connect Wi-Fi ADB"
$connected = $false
for($i=0; $i -lt 10; $i++){
  & $adb connect $wifi | Out-Host
  Start-Sleep -Milliseconds 700
  try {
    & $adb -s $wifi shell "echo ok" | Out-Null
    $connected = $true
    break
  } catch {
    Start-Sleep -Seconds 1
  }
}
if(-not $connected){ throw "Failed to connect Wi-Fi ADB: $wifi" }
Ok "Wi-Fi ADB connected: $wifi"

# ------------------------------------------------------------
# Collect per-device profile (for PC device list / transforms)
# ------------------------------------------------------------
Step "Collect device profile (for device list)"
$ts = Get-Date -Format 'yyyyMMdd_HHmmss'
$outDir = "C:\MirageWork\ops_snapshots\device_profiles"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

function Sh($cmd){
  return (& $adb -s $wifi shell $cmd 2>$null) -join "`n"
}

$wmSize = Sh "wm size"
$wmDensity = Sh "wm density"
$rotUser = Sh "settings get system user_rotation"
$rotAccel = Sh "settings get system accelerometer_rotation"
$display = Sh "dumpsys display | grep -E 'mCurrentRotation|mBaseDisplayInfo|DisplayDeviceInfo|modeId|fps|RefreshRate' | head -n 120"

$pkgInfo = Sh "for p in com.mirage.accessory com.mirage.capture com.mirage.android; do echo --$p; dumpsys package $p 2>/dev/null | grep -E 'versionName|versionCode|lastUpdateTime' | head -n 6; done"

$profile = [ordered]@{
  name = $Name
  usb_serial = $UsbSerial
  wifi_adb = $wifi
  model = (Sh "getprop ro.product.model").Trim()
  manufacturer = (Sh "getprop ro.product.manufacturer").Trim()
  brand = (Sh "getprop ro.product.brand").Trim()
  android_release = (Sh "getprop ro.build.version.release").Trim()
  sdk = (Sh "getprop ro.build.version.sdk").Trim()
  wm_size = $wmSize.Trim()
  wm_density = $wmDensity.Trim()
  user_rotation = $rotUser.Trim()
  accelerometer_rotation = $rotAccel.Trim()
  display_summary = $display.Trim()
  mirage_packages = $pkgInfo.Trim()
  captured_at = (Get-Date).ToString('s')
}

$outPath = Join-Path $outDir ("{0}_{1}.json" -f ($Name -ne '' ? $Name : $UsbSerial), $ts)
$profile | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 $outPath
Ok "Saved profile: $outPath"

# ------------------------------------------------------------
# Start UIs for permission flows
# ------------------------------------------------------------
Step "Start Accessory UI (optional)"
& $adb -s $wifi shell "am start -n com.mirage.accessory/.ui.AccessoryActivity" | Out-Host

Step "Start Capture UI for MediaProjection permission"
& $adb -s $wifi shell "am start -n com.mirage.capture/.ui.CaptureActivity" | Out-Host
Warn "On device: allow screen capture prompt (MediaProjection)."

# ------------------------------------------------------------
# Initial capture tuning
# ------------------------------------------------------------
Step "Set initial FPS/IDR (main 60 preset)"
& $adb -s $wifi shell "am broadcast -a com.mirage.capture.ACTION_VIDEO_FPS --ei target_fps 60 --ei fps 60" | Out-Host
& $adb -s $wifi shell "am broadcast -a com.mirage.capture.ACTION_VIDEO_IDR" | Out-Host

Step "Quick check"
& $adb -s $wifi shell "dumpsys media_projection | head -n 15" | Out-Host
& $adb -s $wifi shell "ps -A | grep mirage | head -n 10" | Out-Host

Ok "Setup complete for $UsbSerial ($wifi)"
Write-Host "NEXT: switch to AOA run mode in MirageVulkan, then you may disable USB debugging later." -ForegroundColor Yellow
