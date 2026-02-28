
@echo off
REM MirageVulkan起動スクリプト
REM adb.exeをPATHに追加して起動

set ADB_DIR=C:\Users\jun\.local\bin\platform-tools
set PATH=%ADB_DIR%;%PATH%
set MIRAGE_DIR=C:\MirageWork\MirageVulkan\build

echo [MirageVulkan] Starting with ADB path: %ADB_DIR%
cd /d %MIRAGE_DIR%
start mirage_vulkan.exe
echo [MirageVulkan] Launched.
