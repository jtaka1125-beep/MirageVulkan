$exe='C:\MirageWork\MirageVulkan\build\mirage_vulkan_debug.exe'
$wd ='C:\MirageWork\MirageVulkan\build'
Start-Process -FilePath $exe -WorkingDirectory $wd -WindowStyle Normal
Write-Host 'started'
