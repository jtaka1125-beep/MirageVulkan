# Freeze probe for MirageVulkan (captures snapshot when UI becomes non-responding or vkframe stalls)
# Usage (PowerShell):
#   powershell -ExecutionPolicy Bypass -File C:\MirageWork\MirageVulkan\tools\freeze_probe.ps1
# Stops with Ctrl+C

$ProcName = "MirageVulkan"
$LogPath  = "C:\MirageWork\MirageVulkan\logs\mirage_vulkan.log"
$OutDir   = "C:\MirageWork\MirageVulkan\logs\freeze"
$IntervalMs = 1000
$StallSeconds = 5

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Get-LastVkframeLine {
  param([string]$path)
  if (-not (Test-Path $path)) { return $null }
  try {
    $tail = Get-Content $path -Tail 200 -ErrorAction Stop
    # pick last line that contains [vkframe]
    $vk = $tail | Where-Object { $_ -match "\[vkframe\]" } | Select-Object -Last 1
    return $vk
  } catch { return $null }
}

function Parse-TimeFromLogLine {
  param([string]$line)
  if (-not $line) { return $null }
  # expects HH:mm:ss.fff at start
  if ($line -match "^(\d{2}:\d{2}:\d{2}\.\d{3})") {
    try { return [DateTime]::ParseExact($Matches[1], "HH:mm:ss.fff", $null) } catch { return $null }
  }
  return $null
}

$lastVkLine = $null
$lastVkTime = $null
$lastWall   = Get-Date

Write-Host "[freeze_probe] watching $ProcName, interval=${IntervalMs}ms, stall=${StallSeconds}s"

while ($true) {
  $p = Get-Process $ProcName -ErrorAction SilentlyContinue
  if (-not $p) {
    Write-Host "[freeze_probe] process not running"
    Start-Sleep -Milliseconds $IntervalMs
    continue
  }

  $responding = $true
  try { $responding = $p.Responding } catch { $responding = $true }

  $vk = Get-LastVkframeLine -path $LogPath
  if ($vk -and $vk -ne $lastVkLine) {
    $lastVkLine = $vk
    $lastWall = Get-Date
  }

  $stall = ((Get-Date) - $lastWall).TotalSeconds -ge $StallSeconds

  if (-not $responding -or $stall) {
    $ts = Get-Date -Format "yyyyMMdd_HHmmss"
    $snap = Join-Path $OutDir "freeze_${ts}.txt"

    "==== Freeze Snapshot $ts ====" | Out-File $snap -Encoding UTF8
    "Responding: $responding" | Out-File $snap -Append -Encoding UTF8
    "vkframe_stall_seconds: $([int](((Get-Date)-$lastWall).TotalSeconds))" | Out-File $snap -Append -Encoding UTF8

    "\n== Process ==" | Out-File $snap -Append
    $p | Select-Object Id,CPU,WS,Threads,StartTime | Format-List | Out-String | Out-File $snap -Append -Encoding UTF8

    "\n== Top Threads (by CPU) ==" | Out-File $snap -Append
    try {
      $p.Threads | Sort-Object CPU -Descending | Select-Object -First 40 Id,ThreadState,WaitReason,CPU | Format-Table -Auto | Out-String | Out-File $snap -Append -Encoding UTF8
    } catch {
      "(thread list unavailable: $($_.Exception.Message))" | Out-File $snap -Append -Encoding UTF8
    }

    "\n== Tail log (200 lines) ==" | Out-File $snap -Append
    if (Test-Path $LogPath) {
      Get-Content $LogPath -Tail 200 | Out-File $snap -Append -Encoding UTF8
    } else {
      "(log missing: $LogPath)" | Out-File $snap -Append -Encoding UTF8
    }

    Write-Host "[freeze_probe] snapshot saved: $snap"
    # After a snapshot, wait a bit to avoid spamming
    Start-Sleep -Seconds 10
  }

  Start-Sleep -Milliseconds $IntervalMs
}
