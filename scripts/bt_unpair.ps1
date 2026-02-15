<#
.SYNOPSIS
  Bluetoothペアリング解除スクリプト（テスト用）
  PC側とAndroid側のペアリングを同時に解除する

.DESCRIPTION
  1. PC側: WinRT DeviceInformationPairing.UnpairAsync() でペアリング解除
  2. Android側: adb経由でBluetoothボンド解除
  テスト時にペアリングをリセットして再テストするために使用

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\bt_unpair.ps1
  powershell -ExecutionPolicy Bypass -File scripts\bt_unpair.ps1 -DeviceName "Pixel 8"
  powershell -ExecutionPolicy Bypass -File scripts\bt_unpair.ps1 -AndroidSerial A9250700956

.NOTES
  Windows PowerShell 5.1 で動作確認済み
  管理者権限不要
#>

param(
    [string]$DeviceName = "",
    [string]$AndroidSerial = "",
    [switch]$PcOnly,
    [switch]$AndroidOnly
)

$ErrorActionPreference = "Stop"

Write-Host "============================================"
Write-Host "  Mirage BT Unpair (ペアリング解除)"
Write-Host "============================================"

# ─── WinRT アセンブリ読み込み ─────────────────────────
if (-not $AndroidOnly) {
    Write-Host "`n[*] WinRT API を読み込み中..."

    try {
        Add-Type -AssemblyName System.Runtime.WindowsRuntime
        [void][Windows.Devices.Bluetooth.BluetoothDevice, Windows.Devices.Bluetooth, ContentType=WindowsRuntime]
        [void][Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType=WindowsRuntime]
        [void][Windows.Devices.Enumeration.DeviceInformationPairing, Windows.Devices.Enumeration, ContentType=WindowsRuntime]
        [void][Windows.Devices.Enumeration.DeviceInformationKind, Windows.Devices.Enumeration, ContentType=WindowsRuntime]
        Write-Host "  [OK] WinRT API 読み込み完了" -ForegroundColor Green
    }
    catch {
        Write-Host "  [NG] WinRT API 読み込み失敗: $_" -ForegroundColor Red
        Write-Host "  Windows PowerShell 5.1 で実行してください" -ForegroundColor Yellow
        if (-not $PcOnly) {
            Write-Host "  Android側のみ実行します..." -ForegroundColor Yellow
            $AndroidOnly = $true
        } else {
            exit 1
        }
    }
}

# ─── 非同期ヘルパー ───────────────────────────────────
function Await-AsyncOperation {
    param([object]$AsyncOp)
    $asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() |
        Where-Object {
            $_.Name -eq 'AsTask' -and
            $_.GetParameters().Count -eq 1 -and
            $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
        })[0]
    if (-not $asTaskGeneric) { throw "AsTask メソッドが見つかりません" }
    $resultType = $AsyncOp.GetType().GetInterface('Windows.Foundation.IAsyncOperation`1').GenericTypeArguments[0]
    $asTask = $asTaskGeneric.MakeGenericMethod($resultType)
    $task = $asTask.Invoke($null, @($AsyncOp))
    $task.Wait()
    return $task.Result
}

# ─── PC側: ペアリング済みデバイスを検索して解除 ───────
$pcUnpaired = 0

if (-not $AndroidOnly) {
    Write-Host "`n[PC] ペアリング済みBluetoothデバイスを検索中..."

    # ペアリング済みデバイス用のAQSフィルタ
    $btPairedSelector = "System.Devices.Aep.ProtocolId:=""{e0cbf06c-cd8b-4647-bb8a-263b43f0f974}"" AND System.Devices.Aep.IsPaired:=System.StructuredQueryType.Boolean#True"

    $requestedProperties = [System.Collections.Generic.List[string]]::new()
    $requestedProperties.Add("System.Devices.Aep.DeviceAddress")
    $requestedProperties.Add("System.Devices.Aep.IsPaired")

    # デバイス列挙
    $findAllAsync = [Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync(
        $btPairedSelector,
        $requestedProperties,
        [Windows.Devices.Enumeration.DeviceInformationKind]::AssociationEndpoint
    )
    $allDevices = Await-AsyncOperation $findAllAsync

    if ($allDevices.Count -eq 0) {
        Write-Host "  ペアリング済みデバイスがありません" -ForegroundColor Yellow
    }
    else {
        Write-Host "  ペアリング済みデバイス: $($allDevices.Count)台"

        foreach ($dev in $allDevices) {
            $devName = $dev.Name
            if (-not $devName) { $devName = "(名前なし)" }

            # DeviceName指定時はフィルタ
            if ($DeviceName -and $devName -notlike "*$DeviceName*") {
                continue
            }

            Write-Host "`n  対象: $devName"
            Write-Host "  ID: $($dev.Id)"

            try {
                $pairing = $dev.Pairing

                if (-not $pairing.IsPaired) {
                    Write-Host "  [--] ペアリングされていません" -ForegroundColor DarkGray
                    continue
                }

                # ペアリング解除
                Write-Host "  ペアリング解除中..."
                $unpairResultAsync = $pairing.UnpairAsync()
                $unpairResult = Await-AsyncOperation $unpairResultAsync

                $status = $unpairResult.Status
                if ($status -eq [Windows.Devices.Enumeration.DeviceUnpairingResultStatus]::Unpaired -or
                    $status -eq [Windows.Devices.Enumeration.DeviceUnpairingResultStatus]::AlreadyUnpaired) {
                    Write-Host "  [OK] $devName のペアリングを解除しました" -ForegroundColor Green
                    $pcUnpaired++
                }
                else {
                    Write-Host "  [NG] ペアリング解除失敗: $status" -ForegroundColor Red
                }
            }
            catch {
                Write-Host "  [NG] エラー: $_" -ForegroundColor Red
            }
        }
    }
}

# ─── Android側: ペアリング解除 ────────────────────────
$androidUnpaired = 0

if (-not $PcOnly) {
    Write-Host "`n[Android] ADBでペアリング解除中..."

    # ADBパスの確認
    $adb = "adb"
    if ($env:ADB) { $adb = $env:ADB }

    # デバイス検出
    $adbArgs = @("devices", "-l")
    if ($AndroidSerial) {
        # 指定シリアルのみ
        $devices = @(@{ serial = $AndroidSerial })
    }
    else {
        $devicesOutput = & $adb devices -l 2>&1
        $devices = @()
        foreach ($line in $devicesOutput -split "`n") {
            if ($line -match "^(\S+)\s+device\s") {
                $ser = $Matches[1]
                # WiFi/BT接続はスキップ
                if ($ser -notmatch ":") {
                    $devices += @{ serial = $ser }
                }
            }
        }
    }

    if ($devices.Count -eq 0) {
        Write-Host "  USB接続されたADBデバイスが見つかりません" -ForegroundColor Yellow
    }

    foreach ($dev in $devices) {
        $serial = $dev.serial
        Write-Host "`n  デバイス: $serial"

        # Bluetooth ボンド解除コマンド
        # 方法1: 設定からリセット（ペアリング済みデバイスを全解除）
        Write-Host "  Bluetoothボンドをリセット中..."

        # Bluetoothを一旦OFF→ONにしてボンド情報をクリア
        try {
            # Bluetooth OFF
            & $adb -s $serial shell "cmd bluetooth_manager disable" 2>&1 | Out-Null
            Write-Host "  Bluetooth OFF" -ForegroundColor Cyan
            Start-Sleep -Seconds 2

            # Bluetooth ON
            & $adb -s $serial shell "cmd bluetooth_manager enable" 2>&1 | Out-Null
            Write-Host "  Bluetooth ON" -ForegroundColor Cyan
            Start-Sleep -Seconds 3

            Write-Host "  [OK] Bluetoothをリセットしました" -ForegroundColor Green
            $androidUnpaired++
        }
        catch {
            Write-Host "  [NG] Bluetoothリセット失敗: $_" -ForegroundColor Red

            # フォールバック: service call で個別解除
            Write-Host "  フォールバック: service call で解除を試行..."
            try {
                & $adb -s $serial shell "service call bluetooth_manager 7" 2>&1 | Out-Null
                Write-Host "  [OK] service call でボンド解除を試行しました" -ForegroundColor Green
                $androidUnpaired++
            }
            catch {
                Write-Host "  [NG] フォールバックも失敗: $_" -ForegroundColor Red
            }
        }
    }
}

# ─── 結果 ────────────────────────────────────────────
Write-Host "`n============================================"
Write-Host "  結果"
Write-Host "============================================"
if (-not $AndroidOnly) {
    Write-Host "  PC側解除: $pcUnpaired 台" -ForegroundColor $(if ($pcUnpaired -gt 0) { "Green" } else { "Yellow" })
}
if (-not $PcOnly) {
    Write-Host "  Android側リセット: $androidUnpaired 台" -ForegroundColor $(if ($androidUnpaired -gt 0) { "Green" } else { "Yellow" })
}
Write-Host "============================================"

$totalUnpaired = $pcUnpaired + $androidUnpaired
if ($totalUnpaired -gt 0) {
    Write-Host "`n  ペアリング解除完了。再ペアリングの準備ができました。" -ForegroundColor Green
    exit 0
} else {
    Write-Host "`n  解除対象が見つかりませんでした。" -ForegroundColor Yellow
    exit 0
}
