path = r'C:\MirageWork\MirageVulkan\android\capture\src\main\AndroidManifest.xml'
with open(path, 'rb') as f:
    raw = f.read()

# Detect line endings
crlf = b'\r\n' in raw
sep = b'\r\n' if crlf else b'\n'
text = raw.decode('utf-8')

changes = []

# 1. Add RECEIVE_BOOT_COMPLETED permission after REQUEST_IGNORE_BATTERY_OPTIMIZATIONS
PERM_ANCHOR = 'android.permission.REQUEST_IGNORE_BATTERY_OPTIMIZATIONS" />'
BOOT_PERM = '    <uses-permission android:name="android.permission.RECEIVE_BOOT_COMPLETED" />'
if 'RECEIVE_BOOT_COMPLETED' not in text:
    text = text.replace(PERM_ANCHOR, PERM_ANCHOR + '\n' + BOOT_PERM, 1)
    changes.append('Added RECEIVE_BOOT_COMPLETED')
else:
    changes.append('RECEIVE_BOOT_COMPLETED already present')

# 2. WatchdogService: add foregroundServiceType
OLD_WDG = '        <service\n            android:name=".svc.WatchdogService"\n            android:exported="false" />'
NEW_WDG = '        <service\n            android:name=".svc.WatchdogService"\n            android:exported="false"\n            android:foregroundServiceType="shortService" />'
if OLD_WDG in text:
    text = text.replace(OLD_WDG, NEW_WDG, 1)
    changes.append('WatchdogService shortService added')
elif 'foregroundServiceType' in text and 'WatchdogService' in text:
    changes.append('WatchdogService type already set')
else:
    # Try CRLF variant
    OLD_WDG_CR = OLD_WDG.replace('\n', '\r\n')
    NEW_WDG_CR = NEW_WDG.replace('\n', '\r\n')
    if OLD_WDG_CR in text:
        text = text.replace(OLD_WDG_CR, NEW_WDG_CR, 1)
        changes.append('WatchdogService shortService added (CRLF)')
    else:
        changes.append('WARNING: WatchdogService entry not matched, skipping')

# 3. Add CaptureBootReceiver before </application>
BOOT_RECV = '''
        <receiver
            android:name=".boot.CaptureBootReceiver"
            android:exported="true"
            android:directBootAware="false">
            <intent-filter>
                <action android:name="android.intent.action.BOOT_COMPLETED" />
                <action android:name="android.intent.action.QUICKBOOT_POWERON" />
                <action android:name="com.mirage.capture.TEST_BOOT" />
            </intent-filter>
        </receiver>

    </application>'''
if 'CaptureBootReceiver' not in text:
    text = text.replace('    </application>', BOOT_RECV, 1)
    changes.append('CaptureBootReceiver added')
else:
    changes.append('CaptureBootReceiver already present')

with open(path, 'wb') as f:
    f.write(text.encode('utf-8'))

print('SAVED')
for c in changes:
    print(' ', c)

# Verify
with open(path, 'rb') as f:
    check = f.read().decode('utf-8')
print('VERIFY BOOT_PERM:', 'RECEIVE_BOOT_COMPLETED' in check)
print('VERIFY BootReceiver:', 'CaptureBootReceiver' in check)
print('VERIFY shortService:', 'shortService' in check)
