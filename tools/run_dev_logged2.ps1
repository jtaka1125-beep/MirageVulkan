$exe = "C:\MirageWork\MirageVulkan\build\mirage_vulkan_debug_dev.exe"
$logDir = "C:\MirageWork\MirageVulkan\logs"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$ts = Get-Date -Format "yyyyMMdd_HHmmss"
$stdout = Join-Path $logDir "dev_stdout_$ts.log"
$stderr = Join-Path $logDir "dev_stderr_$ts.log"
cmd /c "taskkill /F /IM mirage_vulkan_debug_dev.exe 2>nul"
Start-Sleep -Milliseconds 800
Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -RedirectStandardOutput $stdout -RedirectStandardError $stderr
Write-Host "STDOUT: $stdout"
Write-Host "STDERR: $stderr"
