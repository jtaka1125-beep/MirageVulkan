$err='C:\MirageWork\MirageVulkan\logs\last_remote_run_err.txt'
Write-Host '== InputHash lines =='
if (Test-Path $err) {
  Select-String -Path $err -Pattern 'InputHash' -SimpleMatch | Select-Object -Last 30 | ForEach-Object { $_.Line }
} else {
  Write-Host 'missing'
}
