p = r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\access\MirageAccessibilityService.kt'

with open(p,'rb') as f:
    content = f.read().decode('utf-8')

errors = []

def apply(label, old, new):
    global content
    old_crlf = old.replace('\n', '\r\n')
    if old_crlf in content:
        content = content.replace(old_crlf, new.replace('\n', '\r\n'))
        print(f"  {label}: OK")
        return True
    elif old in content:
        content = content.replace(old, new)
        print(f"  {label}: OK (LF)")
        return True
    else:
        print(f"  {label}: ERROR")
        errors.append(label)
        return False

print("=== FIX-4: commandReceiver 削除 ===")

# 1. 不要なimport削除
apply('import BroadcastReceiver',
    'import android.content.BroadcastReceiver\n',
    '')
apply('import IntentFilter',
    'import android.content.IntentFilter\n',
    '')
apply('import LocalBroadcastManager',
    'import androidx.localbroadcastmanager.content.LocalBroadcastManager\n',
    '')

# 2. commandReceiverフィールド削除
apply('commandReceiver field',
    '''    private val commandReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent?.action != AccessoryIoService.ACTION_COMMAND) return
            val cmdType = intent.getIntExtra(AccessoryIoService.EXTRA_COMMAND_TYPE, -1)
            val seq = intent.getIntExtra(AccessoryIoService.EXTRA_SEQ, 0)
            val x = intent.getIntExtra(AccessoryIoService.EXTRA_X, 0)
            val y = intent.getIntExtra(AccessoryIoService.EXTRA_Y, 0)
            val keycode = intent.getIntExtra(AccessoryIoService.EXTRA_KEYCODE, 0)
            Log.d(TAG, "Received USB command: type=$cmdType seq=$seq x=$x y=$y")
            when (cmdType) {
                AccessoryIoService.CMD_TYPE_PING -> Log.i(TAG, "PING received seq=$seq")
                AccessoryIoService.CMD_TYPE_TAP -> { Log.i(TAG, "TAP x=$x y=$y seq=$seq"); tap(x.toFloat(), y.toFloat(), seq) }
                AccessoryIoService.CMD_TYPE_BACK -> { Log.i(TAG, "BACK seq=$seq"); performBack(seq) }
                AccessoryIoService.CMD_TYPE_KEY -> Log.i(TAG, "KEY keycode=$keycode seq=$seq")
            }
        }
    }

    override fun onServiceConnected() {''',
    '    override fun onServiceConnected() {')

# 3. onServiceConnected内のregisterReceiver削除
apply('registerReceiver in onServiceConnected',
    '''        udpSender.start()
        val filter = IntentFilter(AccessoryIoService.ACTION_COMMAND)
        LocalBroadcastManager.getInstance(this).registerReceiver(commandReceiver, filter)
        Log.i(TAG, "AccessibilityService connected and receiver registered")''',
    '''        udpSender.start()
        Log.i(TAG, "AccessibilityService connected")''')

# 4. onDestroy内のunregisterReceiver削除
apply('unregisterReceiver in onDestroy',
    '        LocalBroadcastManager.getInstance(this).unregisterReceiver(commandReceiver)\n        udpSender.stop()',
    '        udpSender.stop()')

print("\n=== FIX-6: MediaProjection packageName 緩和 ===")

# 5. onAccessibilityEvent - packageName完全一致 → 部分一致リスト
apply('onAccessibilityEvent packageName check',
    '''    override fun onAccessibilityEvent(event: AccessibilityEvent?) {
        event ?: return
        // MediaProjectionダイアログの自動承認を試行
        if (event.packageName == "com.android.systemui"
            && event.eventType == AccessibilityEvent.TYPE_WINDOW_STATE_CHANGED
        ) {
            handleMediaProjectionDialog()
        }
    }''',
    '''    override fun onAccessibilityEvent(event: AccessibilityEvent?) {
        event ?: return
        // FIX-6: AOSP + Samsung/MIUI/Huawei 等メーカーROM に対応する部分一致チェック
        val pkg = event.packageName?.toString() ?: return
        val isSystemOverlay = event.eventType == AccessibilityEvent.TYPE_WINDOW_STATE_CHANGED
            && SYSTEM_DIALOG_PACKAGES.any { pkg.contains(it, ignoreCase = true) }
        if (isSystemOverlay) {
            handleMediaProjectionDialog()
        }
    }''')

# 6. companion object に SYSTEM_DIALOG_PACKAGES 追加
apply('SYSTEM_DIALOG_PACKAGES in companion',
    '''    companion object {
        private const val TAG = "MirageA11y"

        @Volatile
        var instance: MirageAccessibilityService? = null
            private set
    }''',
    '''    companion object {
        private const val TAG = "MirageA11y"

        @Volatile
        var instance: MirageAccessibilityService? = null
            private set

        // FIX-6: AOSP + 主要メーカーROMを網羅する部分一致リスト
        private val SYSTEM_DIALOG_PACKAGES = listOf(
            "systemui",              // AOSP / Pixel / Sony
            "android.server",        // AOSP permissions dialog
            "permissioncontroller",  // Android 10+
            "packageinstaller",      // AOSP
            "samsung.android",       // Samsung OneUI
            "miui",                  // Xiaomi MIUI
            "emui",                  // Huawei EMUI
            "oppo",                  // OPPO / Realme
            "oneplus",               // OnePlus OxygenOS
        )
    }''')

# 7. handleMediaProjectionDialog - 承認テキスト拡充 + 親クリック深さ制限
apply('handleMediaProjectionDialog targets',
    '''        // 日本語・英語両方のボタンテキストを検索
        val targets = listOf("開始", "Start", "Start now")
        for (label in targets) {
            val nodes = root.findAccessibilityNodeInfosByText(label)
            if (nodes.isNullOrEmpty()) continue
            for (node in nodes) {
                if (node.isClickable) {
                    node.performAction(AccessibilityNodeInfo.ACTION_CLICK)
                    Log.i(TAG, "MediaProjection自動承認: $label")
                    return
                }
                // ボタン自体がクリック不可の場合、親を辿ってクリック可能なノードを探す
                var parent = node.parent
                while (parent != null) {
                    if (parent.isClickable) {
                        parent.performAction(AccessibilityNodeInfo.ACTION_CLICK)
                        Log.i(TAG, "MediaProjection自動承認(親): $label")
                        return
                    }
                    parent = parent.parent
                }
            }
        }''',
    '''        // FIX-6: 日本語・英語・中国語ボタンテキストを拡充
        val targets = listOf(
            "開始", "Start", "Start now",
            "Allow", "許可", "同意", "はい", "OK",
        )
        for (label in targets) {
            val nodes = root.findAccessibilityNodeInfosByText(label)
            if (nodes.isNullOrEmpty()) continue
            for (node in nodes) {
                if (node.isClickable) {
                    node.performAction(AccessibilityNodeInfo.ACTION_CLICK)
                    Log.i(TAG, "MediaProjection自動承認: $label")
                    return
                }
                // 親を最大5段遡ってクリック可能なノードを探す
                var parent = node.parent
                var depth = 0
                while (parent != null && depth < 5) {
                    if (parent.isClickable) {
                        parent.performAction(AccessibilityNodeInfo.ACTION_CLICK)
                        Log.i(TAG, "MediaProjection自動承認(親 depth=$depth): $label")
                        return
                    }
                    parent = parent.parent
                    depth++
                }
            }
        }''')

if not errors:
    with open(p,'wb') as f:
        f.write(content.encode('utf-8'))
    print("\nAll changes saved OK")
else:
    print(f"\nFailed: {errors}")
