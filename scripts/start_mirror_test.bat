@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul 2>&1

echo ===========================================================
echo  MirageComplete ミラーリングテスト ワンクリック起動
echo ===========================================================
echo.

REM === PCのIPアドレスを自動取得 ===
set "PC_IP="
for /f "tokens=2 delims=:" %%a in ('ipconfig ^| findstr /R /C:"IPv4"') do (
    if not defined PC_IP (
        for /f "tokens=1" %%b in ("%%a") do set "PC_IP=%%b"
    )
)

if not defined PC_IP (
    echo [エラー] IPアドレスを取得できませんでした。
    echo   手動で PC_IP を設定してください。
    pause
    exit /b 1
)

echo [情報] PC IPアドレス: %PC_IP%
echo [情報] 受信ポート  : 50000
echo.

REM === Step 1: appops権限付与 ===
echo [1/4] appops権限を付与中...
adb shell appops set com.mirage.capture PROJECT_MEDIA allow
if %errorlevel% neq 0 (
    echo [警告] appops設定に失敗しました。デバイスが接続されているか確認してください。
) else (
    echo       完了
)
echo.

REM === Step 2: CaptureActivityをauto_mirrorインテントで起動 ===
echo [2/4] CaptureActivityを起動中 (host=%PC_IP%, port=50000)...
adb shell am start -n com.mirage.capture/.ui.CaptureActivity ^
    --ez auto_mirror true ^
    --es mirror_host %PC_IP% ^
    --ei mirror_port 50000
if %errorlevel% neq 0 (
    echo [エラー] Activity起動に失敗しました。
    echo   - APKがインストールされているか確認してください
    echo   - adb devicesでデバイスが見えるか確認してください
    pause
    exit /b 1
) else (
    echo       完了
)
echo.

REM === Step 3: 3秒待機 ===
echo [3/4] MediaProjectionダイアログ待機中 (3秒)...
echo       ※ Android端末で「今すぐ開始」をタップしてください
timeout /t 3 /nobreak >nul
echo       完了
echo.

REM === Step 4: Python UDP受信テスト起動 ===
echo [4/4] UDP受信テストを起動中...
echo ===========================================================
echo.
python "%~dp0test_udp_receiver.py" --port 50000 --timeout 30

echo.
echo テスト終了。
pause
