@echo off
set ANDROID_HOME=C:\Users\jun\AppData\Local\Android\Sdk
set JAVA_HOME=C:\Program Files\Eclipse Adoptium\jdk-17.0.13.11-hotspot
set GRADLE=C:\Users\jun\.gradle\wrapper\dists\gradle-8.13-bin\5xuhj0ry160q40clulazy9h7d\gradle-8.13\bin\gradle.bat
cd /d C:\MirageWork\MirageVulkan\android\MirageStreamer
echo === Building MirageStreamer APK ===
call "%GRADLE%" assembleDebug --no-daemon
