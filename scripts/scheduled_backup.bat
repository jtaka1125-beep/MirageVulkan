@echo off
REM ============================================================
REM scheduled_backup.bat - device_backup.py 定期実行用
REM 
REM タスクスケジューラ登録:
REM   schtasks /create /tn "MirageDeviceBackup" /tr "C:\MirageWork\MirageVulkan\scripts\scheduled_backup.bat" /sc daily /st 03:00
REM
REM 手動実行: このbatをダブルクリック
REM ============================================================

echo [%date% %time%] MirageSystem Device Backup 開始 >> C:\MirageWork\device_backup\backup_log.txt

cd /d C:\MirageWork\MirageVulkan\scripts
python device_backup.py --no-ab >> C:\MirageWork\device_backup\backup_log.txt 2>&1

echo [%date% %time%] 完了 >> C:\MirageWork\device_backup\backup_log.txt
echo. >> C:\MirageWork\device_backup\backup_log.txt
