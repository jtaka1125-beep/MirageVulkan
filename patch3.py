path = r'C:\MirageWork\MirageVulkan\android\settings.gradle.kts'
with open(path, 'rb') as f:
    text = f.read().decode('utf-8')

OLD = 'rootProject.name = "MirageAndroid"\r\ninclude(":app")         // Legacy unified app (deprecated)\r\ninclude(":capture")     // MirageCapture - screen capture + video sending\r\ninclude(":accessory")   // MirageAccessory - AOA + command receiving'

NEW = 'rootProject.name = "MirageAndroid"\r\n// :app (Legacy unified monolith) excluded 2026-02-24. Replaced by :capture + :accessory.\r\n// Sources remain in android/app/ for reference. Do NOT re-include without discussion.\r\ninclude(":capture")     // MirageCapture - screen capture + video sending\r\ninclude(":accessory")   // MirageAccessory - AOA + command receiving'

if OLD in text:
    text = text.replace(OLD, NEW, 1)
    with open(path, 'wb') as f:
        f.write(text.encode('utf-8'))
    print('OK: :app excluded from settings.gradle.kts')
    with open(path,'rb') as f: print(f.read().decode('utf-8'))
else:
    print('ERROR: not found')
