Set-Location "C:\MirageWork\MirageVulkan\build"
$log = "mirage_run_{0}.log" -f (Get-Date -Format "yyyyMMdd_HHmmss")

# PowerShell 5.1: avoid Start-Process stdout/stderr redirection limitations by delegating redirection to cmd.exe.
$exe = Join-Path (Get-Location) "mirage_vulkan_debug.exe"
$cmd = '/c ""{0}" > "{1}" 2>&1"' -f $exe, $log
Start-Process -FilePath "cmd.exe" -ArgumentList $cmd -WorkingDirectory (Get-Location) -WindowStyle Hidden

Write-Output $log
