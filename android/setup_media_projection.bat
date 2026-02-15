@echo off
echo === MirageCapture MediaProjection自動許可設定 ===
echo.
echo captureアプリにPROJECT_MEDIA権限を付与中...
adb shell cmd appops set --user 0 com.mirage.capture PROJECT_MEDIA allow
echo accessoryアプリにPROJECT_MEDIA権限を付与中...
adb shell cmd appops set --user 0 com.mirage.accessory PROJECT_MEDIA allow
echo.
echo 権限確認:
adb shell appops get com.mirage.capture PROJECT_MEDIA
adb shell appops get com.mirage.accessory PROJECT_MEDIA
echo.
echo === 設定完了 ===
pause
