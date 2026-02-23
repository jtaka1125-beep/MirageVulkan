# Launch MirageVulkan debug + freeze probe in interactive session
$BuildDir = 'C:\MirageWork\MirageVulkan\build'
$Exe = Join-Path $BuildDir 'mirage_vulkan_debug.exe'
$Probe = 'C:\MirageWork\MirageVulkan\tools\freeze_probe.ps1'

# Start app
Start-Process -FilePath $Exe -WorkingDirectory $BuildDir -WindowStyle Normal
Start-Sleep -Seconds 2

# Start probe (hidden)
if (Test-Path $Probe) {
  Start-Process -FilePath 'powershell.exe' -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$Probe`"" -WindowStyle Hidden
}
