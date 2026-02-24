# Remote cycle: kill -> single freeze_probe -> launch MirageVulkan with redirected logs
# Retry policy:
#   - Retryable server/network errors: 5s x 12 (竕・0s)
#   - Non-retryable errors: fail fast
# Uses absolute paths for taskkill/wmic to avoid MSYS2 cmd shim issues.

$ErrorActionPreference = 'Stop'

$Exe = 'C:\MirageWork\MirageVulkan\build\mirage_vulkan_debug_dev.exe'
$Wd  = 'C:\MirageWork\MirageVulkan\build'
$Out = 'C:\MirageWork\MirageVulkan\logs\last_remote_run.txt'
$Err = 'C:\MirageWork\MirageVulkan\logs\last_remote_run_err.txt'
$Probe = 'C:\MirageWork\MirageVulkan\tools\freeze_probe.ps1'
$FreezeDir = 'C:\MirageWork\MirageVulkan\logs\freeze'

$Taskkill = "$env:SystemRoot\System32\taskkill.exe"
$Wmic = "$env:SystemRoot\System32\wbem\wmic.exe"

function Is-RetryableError {
  param([string]$msg)
  if (-not $msg) { return $false }
  return (
    $msg -match '524' -or
    $msg -match '502' -or
    $msg -match 'Host Error' -or
    $msg -match 'timeout' -or
    $msg -match 'timed out'
  )
}

function Kill-Image {
  param([string]$Image)
  # Never fail if image is not running
  try {
    if (Test-Path $Taskkill) { & $Taskkill /F /IM $Image *>$null }
  } catch { }
}

function Kill-FreezeProbe {
  try {
    if (Test-Path $Wmic) {
      & $Wmic process where "name='powershell.exe' and CommandLine like '%freeze_probe.ps1%'" call terminate *>$null
    }
  } catch { }
}

function Start-FreezeProbe {
  if (Test-Path $Probe) {
    Start-Process -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -WindowStyle Hidden -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$Probe`"" | Out-Null
  }
}

function Launch-App {
  Remove-Item $Out,$Err -ErrorAction SilentlyContinue
  $p = Start-Process -FilePath $Exe -WorkingDirectory $Wd -PassThru -RedirectStandardOutput $Out -RedirectStandardError $Err
  Start-Sleep -Seconds 2
  return $p
}

function Print-Tail {
  Write-Host '== stderr tail =='
  if (Test-Path $Err) { Get-Content $Err -Tail 120 } else { Write-Host '(missing err)' }
  Write-Host '== freeze snapshots (latest) =='
  if (Test-Path $FreezeDir) {
    Get-ChildItem $FreezeDir -File | Sort-Object LastWriteTime -Descending | Select-Object -First 5 Name,Length,LastWriteTime | Format-Table -Auto
  } else {
    Write-Host 'no freeze dir'
  }
}

# Main loop: 5s x 12 for retryable errors (竕・0s)
for ($i=1; $i -le 12; $i++) {
  try {
    Write-Host "===== Attempt $i/12 ====="

    # 1) Kill any existing app (multi-start prevention)
    Kill-Image 'mirage_vulkan_debug_dev.exe'
    Kill-Image 'MirageVulkan.exe'

    # 2) Ensure single freeze_probe
    Kill-FreezeProbe
    Start-FreezeProbe

    # 3) Launch
    if (-not (Test-Path $Exe)) { throw "EXE missing: $Exe" }
    $p = Launch-App
    Write-Host ("MirageVulkan pid=" + $p.Id + " exited=" + $p.HasExited)
    if ($p.HasExited) { Write-Host ("ExitCode=" + $p.ExitCode) }

    # 4) Quick health (local services)
    try { (Invoke-WebRequest -UseBasicParsing -TimeoutSec 3 http://127.0.0.1:3000/health).StatusCode | Out-Host } catch { Write-Host 'local health FAIL' }
    try { (Invoke-WebRequest -UseBasicParsing -TimeoutSec 3 http://127.0.0.1:20241/ready).StatusCode | Out-Host } catch { Write-Host 'ready20241 FAIL' }

    Print-Tail
    break
  } catch {
    $msg = $_.Exception.Message
    Write-Host ("ERROR: " + $msg)
    Print-Tail

    if (-not (Is-RetryableError $msg)) {
      throw
    }

    if ($i -lt 12) {
      Write-Host 'wait 5s then retry...'
      Start-Sleep -Seconds 5
    } else {
      throw
    }
  }
}
