# DEPRECATED: このスクリプトは旧版です。
# 最新版は auto_setup/bluetooth_adb_setup.py の BluetoothAdbSetup.auto_pair() に統合されました。
# tools/bt_pair_v4.py が最新の自動ペアリング実装です。
<#
.SYNOPSIS
  PC側Bluetoothペアリング完全自動化スクリプト
  WinRT APIを使用してAndroid端末とのペアリングをユーザー操作なしで実行

.DESCRIPTION
  1. FindAllAsyncでBluetooth未ペアリングデバイスを列挙
  2. 指定名またはAndroid端末を自動検出
  3. CustomPairing.PairAsyncでペアリング実行
  4. PairingRequestedハンドラで自動承認（PIN確認ダイアログなし）

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\bt_auto_pair.ps1
  powershell -ExecutionPolicy Bypass -File scripts\bt_auto_pair.ps1 -DeviceName "Pixel 8"
  powershell -ExecutionPolicy Bypass -File scripts\bt_auto_pair.ps1 -TimeoutSeconds 60

.NOTES
  Windows PowerShell 5.1 で動作確認済み
  管理者権限不要
#>

param(
    [string]$DeviceName = "",
    [int]$TimeoutSeconds = 30
)

$ErrorActionPreference = "Stop"

# ─── WinRT アセンブリ読み込み ─────────────────────────
Write-Host "============================================"
Write-Host "  Mirage BT Auto-Pair (PC側)"
Write-Host "============================================"

Write-Host "`n[1/4] WinRT API を読み込み中..."

try {
    # WindowsRuntime拡張メソッド用
    Add-Type -AssemblyName System.Runtime.WindowsRuntime

    # Bluetooth / Enumeration WinRT型をロード
    [void][Windows.Devices.Bluetooth.BluetoothDevice, Windows.Devices.Bluetooth, ContentType=WindowsRuntime]
    [void][Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType=WindowsRuntime]
    [void][Windows.Devices.Enumeration.DeviceInformationPairing, Windows.Devices.Enumeration, ContentType=WindowsRuntime]
    [void][Windows.Devices.Enumeration.DevicePairingResult, Windows.Devices.Enumeration, ContentType=WindowsRuntime]
    [void][Windows.Devices.Enumeration.DevicePairingKinds, Windows.Devices.Enumeration, ContentType=WindowsRuntime]
    [void][Windows.Devices.Enumeration.DeviceInformationKind, Windows.Devices.Enumeration, ContentType=WindowsRuntime]

    Write-Host "  [OK] WinRT API 読み込み完了" -ForegroundColor Green
}
catch {
    Write-Host "  [NG] WinRT API 読み込み失敗: $_" -ForegroundColor Red
    Write-Host "  ヒント: Windows PowerShell 5.1 (powershell.exe) で実行してください" -ForegroundColor Yellow
    Write-Host "  PowerShell 7 (pwsh.exe) ではWinRT APIが使えない場合があります" -ForegroundColor Yellow
    exit 1
}

# ─── 非同期ヘルパー ───────────────────────────────────
# IAsyncOperation<T> をPowerShellで待機するための関数
function Await-AsyncOperation {
    param([object]$AsyncOp)

    # AsTask拡張メソッドを取得（ジェネリック型対応）
    $asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() |
        Where-Object {
            $_.Name -eq 'AsTask' -and
            $_.GetParameters().Count -eq 1 -and
            $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
        })[0]

    if (-not $asTaskGeneric) {
        throw "AsTask メソッドが見つかりません"
    }

    # 結果型を推定
    $resultType = $AsyncOp.GetType().GetInterface('Windows.Foundation.IAsyncOperation`1').GenericTypeArguments[0]
    $asTask = $asTaskGeneric.MakeGenericMethod($resultType)

    $task = $asTask.Invoke($null, @($AsyncOp))
    $task.Wait()
    return $task.Result
}

# ─── Bluetoothデバイス スキャン ───────────────────────
Write-Host "`n[2/4] Bluetoothデバイスをスキャン中..."

if ($DeviceName) {
    Write-Host "  検索対象: '$DeviceName'"
}
else {
    Write-Host "  未ペアリングのデバイスを全て検索"
}

# Bluetooth未ペアリングデバイス用のAQSフィルタ
$btSelector = "System.Devices.Aep.ProtocolId:=""{e0cbf06c-cd8b-4647-bb8a-263b43f0f974}"" AND System.Devices.Aep.IsPaired:=System.StructuredQueryType.Boolean#False"

# 追加プロパティ
$requestedProperties = [System.Collections.Generic.List[string]]::new()
$requestedProperties.Add("System.Devices.Aep.DeviceAddress")
$requestedProperties.Add("System.Devices.Aep.IsConnected")
$requestedProperties.Add("System.Devices.Aep.IsPaired")

# FindAllAsync で一括列挙（DeviceWatcherより確実）
Write-Host "  デバイス列挙中（タイムアウト: ${TimeoutSeconds}秒）..."

$findAllAsync = [Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync(
    $btSelector,
    $requestedProperties,
    [Windows.Devices.Enumeration.DeviceInformationKind]::AssociationEndpoint
)
$allDevices = Await-AsyncOperation $findAllAsync

# フィルタリング
$foundDevices = [System.Collections.ArrayList]::new()
foreach ($devInfo in $allDevices) {
    $devName = $devInfo.Name
    if (-not $devName) { continue }

    # DeviceName指定時はフィルタ
    if ($DeviceName -and $devName -notlike "*${DeviceName}*") {
        continue
    }

    $entry = @{
        Id   = $devInfo.Id
        Name = $devName
    }
    [void]$foundDevices.Add($entry)
    Write-Host "  発見: $devName" -ForegroundColor Cyan
}

if ($foundDevices.Count -eq 0) {
    Write-Host "  [NG] 未ペアリングのBluetoothデバイスが見つかりません" -ForegroundColor Red
    if ($DeviceName) {
        Write-Host "  ヒント: デバイス名 '$DeviceName' が正しいか確認してください" -ForegroundColor Yellow
    }
    Write-Host "  ヒント: Android側でBluetooth設定画面を開いて検出可能にしてください" -ForegroundColor Yellow
    exit 1
}

Write-Host "`n  検出デバイス数: $($foundDevices.Count)" -ForegroundColor Green
foreach ($dev in $foundDevices) {
    Write-Host "    - $($dev.Name) [$($dev.Id)]"
}

# ─── ペアリング実行 ───────────────────────────────────
Write-Host "`n[3/4] ペアリングを実行中..."

$pairedCount = 0
$failedCount = 0

foreach ($dev in $foundDevices) {
    $targetName = $dev.Name
    $targetId = $dev.Id

    Write-Host "`n  対象: $targetName"
    Write-Host "  ID: $targetId"

    try {
        # DeviceInformationを取得
        $deviceInfoAsync = [Windows.Devices.Enumeration.DeviceInformation]::CreateFromIdAsync(
            $targetId,
            $requestedProperties,
            [Windows.Devices.Enumeration.DeviceInformationKind]::AssociationEndpoint
        )
        $deviceInfo = Await-AsyncOperation $deviceInfoAsync

        if (-not $deviceInfo) {
            Write-Host "  [NG] デバイス情報の取得に失敗" -ForegroundColor Red
            $failedCount++
            continue
        }

        # カスタムペアリングを使用（自動承認のため）
        $pairing = $deviceInfo.Pairing

        if ($pairing.IsPaired) {
            Write-Host "  [OK] 既にペアリング済みです" -ForegroundColor Green
            $pairedCount++
            continue
        }

        if (-not $pairing.CanPair) {
            Write-Host "  [NG] このデバイスはペアリングできません" -ForegroundColor Red
            $failedCount++
            continue
        }

        $customPairing = $pairing.Custom

        # PairingRequested ハンドラ: 全種類のペアリング要求を自動承認
        # Register-ObjectEvent では $using: スコープが使えないため、
        # -MessageData でコンテキストを渡す必要はないが、
        # ScriptBlock内では $Event, $Sender, $EventArgs 自動変数が使える
        $pairingHandler = {
            $pairingArgs = $EventArgs
            $kind = $pairingArgs.PairingKind
            Write-Host "    ペアリング種別: $kind" -ForegroundColor Cyan

            switch ($kind) {
                "ConfirmOnly" {
                    # 確認のみ → 自動承認
                    Write-Host "    -> 自動承認 (ConfirmOnly)" -ForegroundColor Green
                    $pairingArgs.Accept()
                }
                "ConfirmPinMatch" {
                    # PIN一致確認 → 自動承認
                    $pin = $pairingArgs.Pin
                    Write-Host "    -> PIN: $pin を自動承認" -ForegroundColor Green
                    $pairingArgs.Accept()
                }
                "ProvidePin" {
                    # PIN入力が必要 → デフォルトPIN
                    Write-Host "    -> PIN '0000' を自動入力" -ForegroundColor Green
                    $pairingArgs.Accept("0000")
                }
                "DisplayPin" {
                    # PIN表示のみ（相手側で入力）
                    $pin = $pairingArgs.Pin
                    Write-Host "    -> 表示PIN: $pin (Android側で入力してください)" -ForegroundColor Yellow
                    $pairingArgs.Accept()
                }
                default {
                    Write-Host "    -> 未対応のペアリング種別: $kind" -ForegroundColor Yellow
                    $pairingArgs.Accept()
                }
            }
        }

        # ペアリング種別フラグ（全種類に対応）
        $pairingKinds = [Windows.Devices.Enumeration.DevicePairingKinds]::ConfirmOnly -bor
                        [Windows.Devices.Enumeration.DevicePairingKinds]::ConfirmPinMatch -bor
                        [Windows.Devices.Enumeration.DevicePairingKinds]::ProvidePin -bor
                        [Windows.Devices.Enumeration.DevicePairingKinds]::DisplayPin

        # イベントハンドラ登録
        Register-ObjectEvent -InputObject $customPairing -EventName "PairingRequested" -Action $pairingHandler | Out-Null

        # ペアリング開始
        Write-Host "  ペアリング開始..."
        $pairResultAsync = $customPairing.PairAsync($pairingKinds)
        $pairResult = Await-AsyncOperation $pairResultAsync

        # イベント解除
        Get-EventSubscriber | Where-Object { $_.SourceObject -eq $customPairing } | Unregister-Event -ErrorAction SilentlyContinue

        # 結果判定
        $status = $pairResult.Status
        Write-Host "  ペアリング結果: $status"

        if ($status -eq [Windows.Devices.Enumeration.DevicePairingResultStatus]::Paired) {
            Write-Host "  [OK] $targetName とのペアリング成功！" -ForegroundColor Green
            $pairedCount++
        }
        elseif ($status -eq [Windows.Devices.Enumeration.DevicePairingResultStatus]::AlreadyPaired) {
            Write-Host "  [OK] $targetName は既にペアリング済み" -ForegroundColor Green
            $pairedCount++
        }
        else {
            Write-Host "  [NG] ペアリング失敗: $status" -ForegroundColor Red
            $failedCount++
        }
    }
    catch {
        Write-Host "  [NG] エラー: $_" -ForegroundColor Red
        $failedCount++
    }
}

# ─── 結果サマリー ─────────────────────────────────────
Write-Host "`n[4/4] 結果"
Write-Host "============================================"
Write-Host "  成功: $pairedCount" -ForegroundColor Green
if ($failedCount -gt 0) {
    Write-Host "  失敗: $failedCount" -ForegroundColor Red
}
Write-Host "============================================"

if ($failedCount -gt 0) {
    exit 1
}
exit 0
