@echo off
chcp 65001 >nul
echo ============================================
echo  Bluetooth PAN ADB テスト
echo ============================================
echo.

REM Step 1: A9を検出可能に
echo [Step 1] A9を検出可能モードに設定...
adb -s adb-A9250700956-ieJaCE._adb-tls-connect._tcp shell "am start -a android.bluetooth.adapter.action.REQUEST_DISCOVERABLE --ei android.bluetooth.adapter.extra.DISCOVERABLE_DURATION 300"
timeout /t 2 >nul

REM 許可ダイアログ自動承認
echo [Step 1b] 許可ダイアログを承認...
adb -s adb-A9250700956-ieJaCE._adb-tls-connect._tcp shell "uiautomator dump /sdcard/ui.xml"
timeout /t 1 >nul
adb -s adb-A9250700956-ieJaCE._adb-tls-connect._tcp shell "input tap 640 700"
timeout /t 2 >nul

REM Step 2: WindowsのBluetooth設定を開く
echo.
echo [Step 2] Bluetooth設定を開きます...
echo   1. A9 (MAC: 9C:92:53:DD:20:83) を探してペアリング
echo   2. ペアリング完了したらこのウィンドウに戻ってEnter
echo.
start ms-settings:bluetooth
pause

REM Step 3: ペアリング確認
echo.
echo [Step 3] ペアリング確認...
powershell -Command "Get-PnpDevice -Class Bluetooth | Where-Object { $_.FriendlyName -like '*A9*' } | Format-Table Status, FriendlyName"

REM Step 4: Bluetoothテザリング有効化
echo.
echo [Step 4] Bluetoothテザリングを有効化...
adb -s adb-A9250700956-ieJaCE._adb-tls-connect._tcp shell "svc bluetooth enable"
adb -s adb-A9250700956-ieJaCE._adb-tls-connect._tcp shell "am start -a android.settings.TETHER_SETTINGS"
echo   Androidで「Bluetoothテザリング」を有効にしてください
pause

REM Step 5: BT PAN接続 (Windows側)
echo.
echo [Step 5] Bluetooth PAN接続...
echo   Windows設定でA9のBluetooth PAN接続を確認
powershell -Command "Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -like '192.168.44.*' -or $_.IPAddress -like '192.168.137.*' } | Select-Object IPAddress, InterfaceAlias"

REM Step 6: ADB接続テスト
echo.
echo [Step 6] BT PAN経由ADB接続テスト...
echo   BT PAN上のホストスキャン中...

REM 一般的なBTテザリングIP
for %%i in (192.168.44.1 192.168.44.2 192.168.137.1) do (
    echo   Testing %%i:5555 ...
    adb connect %%i:5555 2>nul | findstr /i "connected already" && (
        echo   [OK] ADB接続成功: %%i:5555
        goto :adb_connected
    )
)

echo [INFO] 自動接続失敗。BT PAN IPを手動入力してください:
set /p BT_IP=IP: 
adb connect %BT_IP%:5555

:adb_connected
echo.
echo [Step 7] ADB接続確認...
adb devices -l
echo.

REM Step 8: TCP映像フォワードテスト
echo [Step 8] TCP映像フォワードテスト...
echo   まずCaptureサービスをTCPモードで起動
adb -s adb-A9250700956-ieJaCE._adb-tls-connect._tcp shell "am start -n com.mirage.capture/.ui.CaptureActivity --ez auto_mirror true --es mirror_host 192.168.0.7 --ei mirror_port 50000 --es mirror_mode tcp"
timeout /t 3 >nul

echo   BT PAN ADB経由でTCPフォワード設定...
REM BT PAN ADB serial を使う
for /f "tokens=1" %%s in ('adb devices ^| findstr "192.168.44\|192.168.137" ^| findstr "device"') do (
    echo   BT ADB serial: %%s
    adb -s %%s forward tcp:50100 tcp:50100
    echo   フォワード設定完了
)

echo.
echo   tcp_recv_test.exe で受信テスト...
cd /d C:\MirageWork\MirageComplete\build
tcp_recv_test.exe 50100 5

echo.
echo ============================================
echo  テスト完了
echo ============================================
pause
