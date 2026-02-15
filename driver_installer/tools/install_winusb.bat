@echo off
chcp 65001 >nul
echo ============================================================
echo   MirageSystem - WinUSB ドライバインストール
echo ============================================================
echo.
echo AOAデバイス用のWinUSBドライバをインストールします。
echo.
echo 手順:
echo   1. Zadigが起動します
echo   2. デバイスリストから VID_18D1 ^& PID_2D01 を選択
echo   3. ドライバが「WinUSB」になっていることを確認
echo   4. 「Install Driver」または「Replace Driver」をクリック
echo   5. 完了したらZadigを閉じてください
echo.
echo ============================================================
echo.

:: 管理者チェック
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] 管理者権限が必要です。
    echo         右クリック→「管理者として実行」で起動してください。
    echo.
    pause
    exit /b 1
)

:: zadig.ini をzadig.exeと同じフォルダにコピー
copy /Y "%~dp0zadig.ini" "%~dp0zadig.ini" >nul 2>&1

:: Zadig起動
echo Zadigを起動中...
start "" "%~dp0zadig.exe"

echo.
echo Zadigでドライバインストールが完了したらこのウィンドウを閉じてください。
echo.
pause
