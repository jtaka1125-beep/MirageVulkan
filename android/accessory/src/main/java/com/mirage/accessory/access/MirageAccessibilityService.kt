package com.mirage.accessory.access

import android.accessibilityservice.AccessibilityService
import android.accessibilityservice.GestureDescription
import android.content.Context
import android.content.Intent
import android.graphics.Path
import android.util.Log
import android.graphics.Rect
import android.view.accessibility.AccessibilityEvent
import android.view.accessibility.AccessibilityNodeInfo
import com.mirage.accessory.core.Config
import com.mirage.accessory.svc.UdpSender
import com.mirage.accessory.usb.AccessoryIoService
import kotlin.math.cos
import kotlin.math.sin

class MirageAccessibilityService : AccessibilityService() {
    companion object {
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

        // ISSUE-8: MediaProjection固有キーワード (他権限ダイアログとの誤認防止)
        private val MEDIA_PROJECTION_HINTS = listOf(
            "Screen capture", "Screen Cast", "画面のキャスト", "画面録画",
            "Cast screen", "MediaProjection", "casting", "screen sharing",
            "MirageCapture", "mirage.capture",
        )
    }

    private val udpSender = UdpSender()

    override fun onServiceConnected() {
        super.onServiceConnected()
        instance = this
        udpSender.start()
        Log.i(TAG, "AccessibilityService connected")
    }

    override fun onAccessibilityEvent(event: AccessibilityEvent?) {
        event ?: return
        // FIX-6: AOSP + Samsung/MIUI/Huawei 等メーカーROM に対応する部分一致チェック
        val pkg = event.packageName?.toString() ?: return
        val isSystemOverlay = event.eventType == AccessibilityEvent.TYPE_WINDOW_STATE_CHANGED
            && SYSTEM_DIALOG_PACKAGES.any { pkg.contains(it, ignoreCase = true) }
        if (isSystemOverlay) {
            // ISSUE-8: MediaProjectionダイアログか確認してから自動承認
            val root = rootInActiveWindow
            if (root != null && isMediaProjectionDialog(root)) {
                handleMediaProjectionDialog()
            }
        }
    }

    /**
     * ISSUE-8: 画面がMediaProjectionダイアログかどうかをヒューリスティックで判定。
     * カメラ/マイク等の他の権限ダイアログを誤クリックしないための防護壁。
     */
    private fun isMediaProjectionDialog(root: android.view.accessibility.AccessibilityNodeInfo): Boolean {
        return MEDIA_PROJECTION_HINTS.any { hint ->
            root.findAccessibilityNodeInfosByText(hint).isNotEmpty()
        } || run {
            val pkg = root.packageName?.toString() ?: ""
            pkg.contains("mirage", ignoreCase = true) || pkg.contains("capture", ignoreCase = true)
        }
    }

    /**
     * MediaProjectionの許可ダイアログを自動承認する。
     * SystemUI上で「開始」「Start」「Start now」ボタンを検索し、
     * 見つかった場合は自動クリックする。
     */
    private fun handleMediaProjectionDialog() {
        val root = rootInActiveWindow ?: return
        // FIX-6: 日本語・英語・中国語ボタンテキストを拡充
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
        }
    }
    override fun onInterrupt() {}

    override fun onDestroy() {
        udpSender.stop()
        instance = null
        super.onDestroy()
    }

    // =========================================================================
    // Gesture implementations
    // =========================================================================

    fun tap(x: Float, y: Float, seq: Int = 0) {
        Log.i(TAG, "tap() called: ($x, $y) seq=$seq")
        val p = Path().apply { moveTo(x, y) }
        val stroke = GestureDescription.StrokeDescription(p, 0, 50)
        val gd = GestureDescription.Builder().addStroke(stroke).build()
        val tStart = System.currentTimeMillis()
        val result = dispatchGesture(gd, object : GestureResultCallback() {
            override fun onCompleted(gestureDescription: GestureDescription?) {
                val latency = System.currentTimeMillis() - tStart
                Log.i(TAG, "Tap COMPLETED ($x,$y) seq=$seq ${latency}ms")
                udpSender.trySendLine(Config.TAG_TAP_EXEC,
                    """{"slot":${Config.DEFAULT_SLOT},"seq":$seq,"x":${x.toInt()},"y":${y.toInt()},"ok":true,"latency_ms":$latency}""")
            }
            override fun onCancelled(gestureDescription: GestureDescription?) {
                Log.w(TAG, "Tap CANCELLED ($x,$y) seq=$seq")
                udpSender.trySendLine(Config.TAG_TAP_EXEC,
                    """{"slot":${Config.DEFAULT_SLOT},"seq":$seq,"x":${x.toInt()},"y":${y.toInt()},"ok":false}""")
            }
        }, null)
        Log.i(TAG, "dispatchGesture returned: $result")
    }

    fun swipe(startX: Float, startY: Float, endX: Float, endY: Float, durationMs: Int, seq: Int = 0) {
        Log.i(TAG, "swipe() ($startX,$startY)->($endX,$endY) dur=$durationMs seq=$seq")
        val duration = durationMs.toLong().coerceIn(50, 60000)
        val p = Path().apply { moveTo(startX, startY); lineTo(endX, endY) }
        val stroke = GestureDescription.StrokeDescription(p, 0, duration)
        val gd = GestureDescription.Builder().addStroke(stroke).build()
        val tStart = System.currentTimeMillis()
        val result = dispatchGesture(gd, object : GestureResultCallback() {
            override fun onCompleted(gestureDescription: GestureDescription?) {
                val latency = System.currentTimeMillis() - tStart
                Log.i(TAG, "Swipe COMPLETED seq=$seq ${latency}ms")
                udpSender.trySendLine(Config.TAG_TAP_EXEC,
                    """{"slot":${Config.DEFAULT_SLOT},"seq":$seq,"type":"swipe","ok":true,"latency_ms":$latency}""")
            }
            override fun onCancelled(gestureDescription: GestureDescription?) {
                Log.w(TAG, "Swipe CANCELLED seq=$seq")
                udpSender.trySendLine(Config.TAG_TAP_EXEC,
                    """{"slot":${Config.DEFAULT_SLOT},"seq":$seq,"type":"swipe","ok":false}""")
            }
        }, null)
        Log.i(TAG, "swipe dispatchGesture returned: $result")
    }

    fun pinch(centerX: Float, centerY: Float, startDist: Int, endDist: Int,
              durationMs: Int, angleDeg100: Int, seq: Int = 0) {
        Log.i(TAG, "pinch() center=($centerX,$centerY) $startDist->$endDist dur=$durationMs seq=$seq")
        val duration = durationMs.toLong().coerceIn(50, 60000)
        val angleRad = (angleDeg100 / 100.0) * Math.PI / 180.0
        val cosA = cos(angleRad).toFloat()
        val sinA = sin(angleRad).toFloat()
        val halfStart = startDist / 2f
        val halfEnd = endDist / 2f
        val p1 = Path().apply {
            moveTo(centerX + halfStart * cosA, centerY + halfStart * sinA)
            lineTo(centerX + halfEnd * cosA, centerY + halfEnd * sinA)
        }
        val p2 = Path().apply {
            moveTo(centerX - halfStart * cosA, centerY - halfStart * sinA)
            lineTo(centerX - halfEnd * cosA, centerY - halfEnd * sinA)
        }
        val s1 = GestureDescription.StrokeDescription(p1, 0, duration)
        val s2 = GestureDescription.StrokeDescription(p2, 0, duration)
        val gd = GestureDescription.Builder().addStroke(s1).addStroke(s2).build()
        val tStart = System.currentTimeMillis()
        val result = dispatchGesture(gd, object : GestureResultCallback() {
            override fun onCompleted(gestureDescription: GestureDescription?) {
                val latency = System.currentTimeMillis() - tStart
                Log.i(TAG, "Pinch COMPLETED seq=$seq ${latency}ms")
                udpSender.trySendLine(Config.TAG_TAP_EXEC,
                    """{"slot":${Config.DEFAULT_SLOT},"seq":$seq,"type":"pinch","ok":true,"latency_ms":$latency}""")
            }
            override fun onCancelled(gestureDescription: GestureDescription?) {
                Log.w(TAG, "Pinch CANCELLED seq=$seq")
                udpSender.trySendLine(Config.TAG_TAP_EXEC,
                    """{"slot":${Config.DEFAULT_SLOT},"seq":$seq,"type":"pinch","ok":false}""")
            }
        }, null)
        Log.i(TAG, "pinch dispatchGesture returned: $result")
    }

    fun longPress(x: Float, y: Float, durationMs: Int, seq: Int = 0) {
        Log.i(TAG, "longPress() ($x,$y) dur=$durationMs seq=$seq")
        val duration = durationMs.toLong().coerceIn(500, 60000)
        val p = Path().apply { moveTo(x, y) }
        val stroke = GestureDescription.StrokeDescription(p, 0, duration)
        val gd = GestureDescription.Builder().addStroke(stroke).build()
        val tStart = System.currentTimeMillis()
        val result = dispatchGesture(gd, object : GestureResultCallback() {
            override fun onCompleted(gestureDescription: GestureDescription?) {
                val latency = System.currentTimeMillis() - tStart
                Log.i(TAG, "LongPress COMPLETED ($x,$y) seq=$seq ${latency}ms")
                udpSender.trySendLine(Config.TAG_TAP_EXEC,
                    """{"slot":${Config.DEFAULT_SLOT},"seq":$seq,"type":"longpress","ok":true,"latency_ms":$latency}""")
            }
            override fun onCancelled(gestureDescription: GestureDescription?) {
                Log.w(TAG, "LongPress CANCELLED seq=$seq")
                udpSender.trySendLine(Config.TAG_TAP_EXEC,
                    """{"slot":${Config.DEFAULT_SLOT},"seq":$seq,"type":"longpress","ok":false}""")
            }
        }, null)
        Log.i(TAG, "longPress dispatchGesture returned: $result")
    }

    fun performBack(seq: Int = 0) {
        val result = performGlobalAction(GLOBAL_ACTION_BACK)
        Log.i(TAG, "BACK result=$result seq=$seq")
        udpSender.trySendLine(Config.TAG_BACK_EXEC,
            """{"slot":${Config.DEFAULT_SLOT},"seq":$seq,"ok":$result}""")
    }

    // ISSUE-19: dispatch arbitrary keycode via global actions or KeyEvent
    fun performKey(keycode: Int, seq: Int = 0) {
        // Map common keycodes to AccessibilityService global actions
        val globalAction = when (keycode) {
            android.view.KeyEvent.KEYCODE_BACK         -> GLOBAL_ACTION_BACK
            android.view.KeyEvent.KEYCODE_HOME         -> GLOBAL_ACTION_HOME
            android.view.KeyEvent.KEYCODE_APP_SWITCH   -> GLOBAL_ACTION_RECENTS
            android.view.KeyEvent.KEYCODE_NOTIFICATION -> GLOBAL_ACTION_NOTIFICATIONS
            else -> -1
        }
        if (globalAction >= 0) {
            val result = performGlobalAction(globalAction)
            Log.i(TAG, "KEY keycode=$keycode globalAction=$globalAction result=$result seq=$seq")
            udpSender.trySendLine(Config.TAG_BACK_EXEC,
                """{"slot":${Config.DEFAULT_SLOT},"seq":$seq,"type":"key","keycode":$keycode,"ok":$result}""")
        } else {
            // Inject raw KeyEvent for keys without global action mappings
            try {
                val im = getSystemService(android.hardware.input.InputManager::class.java)
                val down = android.view.KeyEvent(android.view.KeyEvent.ACTION_DOWN, keycode)
                val up   = android.view.KeyEvent(android.view.KeyEvent.ACTION_UP,   keycode)
                im.injectInputEvent(down, android.hardware.input.InputManager.INJECT_INPUT_EVENT_MODE_ASYNC)
                im.injectInputEvent(up,   android.hardware.input.InputManager.INJECT_INPUT_EVENT_MODE_ASYNC)
                Log.i(TAG, "KEY inject keycode=$keycode seq=$seq")
            } catch (e: Exception) {
                Log.w(TAG, "KEY keycode=$keycode not supported: ${e.message} seq=$seq")
            }
        }
    }

    // =========================================================================
    // リソースID / テキスト指定クリック
    // =========================================================================

    /** リソースIDでノードを検索してクリックする */
    fun clickById(resourceId: String, seq: Int = 0) {
        Log.i(TAG, "clickById() resourceId=$resourceId seq=$seq")
        val root = rootInActiveWindow
        if (root == null) {
            Log.w(TAG, "clickById: rootInActiveWindow is null")
            udpSender.trySendLine(Config.TAG_TAP_EXEC,
                """{"slot":${Config.DEFAULT_SLOT},"seq":$seq,"type":"click_id","ok":false,"reason":"no_root"}""")
            return
        }
        val nodes = root.findAccessibilityNodeInfosByViewId(resourceId)
        val node = nodes?.firstOrNull()
        if (node == null) {
            Log.w(TAG, "clickById: ノードが見つかりません: $resourceId")
            udpSender.trySendLine(Config.TAG_TAP_EXEC,
                """{"slot":${Config.DEFAULT_SLOT},"seq":$seq,"type":"click_id","ok":false,"reason":"not_found"}""")
            return
        }
        performNodeClick(node, "click_id", seq)
    }

    /** テキストでノードを検索してクリックする */
    fun clickByText(text: String, seq: Int = 0) {
        Log.i(TAG, "clickByText() text=$text seq=$seq")
        val root = rootInActiveWindow
        if (root == null) {
            Log.w(TAG, "clickByText: rootInActiveWindow is null")
            udpSender.trySendLine(Config.TAG_TAP_EXEC,
                """{"slot":${Config.DEFAULT_SLOT},"seq":$seq,"type":"click_text","ok":false,"reason":"no_root"}""")
            return
        }
        val nodes = root.findAccessibilityNodeInfosByText(text)
        val node = nodes?.firstOrNull()
        if (node == null) {
            Log.w(TAG, "clickByText: ノードが見つかりません: $text")
            udpSender.trySendLine(Config.TAG_TAP_EXEC,
                """{"slot":${Config.DEFAULT_SLOT},"seq":$seq,"type":"click_text","ok":false,"reason":"not_found"}""")
            return
        }
        performNodeClick(node, "click_text", seq)
    }

    /**
     * ノードをクリックする共通処理。
     * ノード自体がクリック可能ならACTION_CLICK、
     * そうでなければノード中心座標にdispatchGestureでタップする。
     */
    private fun performNodeClick(node: AccessibilityNodeInfo, type: String, seq: Int) {
        if (node.isClickable) {
            val result = node.performAction(AccessibilityNodeInfo.ACTION_CLICK)
            Log.i(TAG, "$type ACTION_CLICK result=$result seq=$seq")
            udpSender.trySendLine(Config.TAG_TAP_EXEC,
                """{"slot":${Config.DEFAULT_SLOT},"seq":$seq,"type":"$type","ok":$result}""")
        } else {
            // ノードの中心座標を計算してジェスチャータップ
            val rect = Rect()
            node.getBoundsInScreen(rect)
            Log.i(TAG, "$type ノード非クリック可能 → ジェスチャータップ (${rect.centerX()}, ${rect.centerY()})")
            performTapGesture(rect.centerX(), rect.centerY(), type, seq)
        }
    }

    /** AccessibilityServiceのdispatchGestureで指定座標にタップする */
    private fun performTapGesture(x: Int, y: Int, type: String = "tap_gesture", seq: Int = 0) {
        val p = Path().apply { moveTo(x.toFloat(), y.toFloat()) }
        val stroke = GestureDescription.StrokeDescription(p, 0, 50)
        val gd = GestureDescription.Builder().addStroke(stroke).build()
        val tStart = System.currentTimeMillis()
        dispatchGesture(gd, object : GestureResultCallback() {
            override fun onCompleted(gestureDescription: GestureDescription?) {
                val latency = System.currentTimeMillis() - tStart
                Log.i(TAG, "$type tap COMPLETED ($x,$y) seq=$seq ${latency}ms")
                udpSender.trySendLine(Config.TAG_TAP_EXEC,
                    """{"slot":${Config.DEFAULT_SLOT},"seq":$seq,"type":"$type","ok":true,"latency_ms":$latency}""")
            }
            override fun onCancelled(gestureDescription: GestureDescription?) {
                Log.w(TAG, "$type tap CANCELLED ($x,$y) seq=$seq")
                udpSender.trySendLine(Config.TAG_TAP_EXEC,
                    """{"slot":${Config.DEFAULT_SLOT},"seq":$seq,"type":"$type","ok":false}""")
            }
        }, null)
    }

    /**
     * UIツリーをJSON形式でダンプして返す (CMD_UI_TREE_REQ ハンドラ用)
     */
    fun dumpUiTree(): String? {
        val root = rootInActiveWindow ?: return null
        return buildJsonNode(root, 0)
    }

    private fun buildJsonNode(node: android.view.accessibility.AccessibilityNodeInfo, depth: Int): String {
        if (depth > 12) return "{"truncated":true}"
        val rect = Rect()
        node.getBoundsInScreen(rect)
        val esc = { s: String -> s.replace("\", "\\").replace(""", "\"") }
        val text = esc(node.text?.toString() ?: "")
        val cd   = esc(node.contentDescription?.toString() ?: "")
        val rid  = node.viewIdResourceName ?: ""
        val sb = StringBuilder("{")
        sb.append(""pkg":"${node.packageName ?: ""}"")
        sb.append(","cls":"${node.className ?: ""}"")
        sb.append(","rid":"$rid"")
        sb.append(","text":"$text"")
        sb.append(","cd":"$cd"")
        sb.append(","click":${node.isClickable}")
        sb.append(","bounds":[${rect.left},${rect.top},${rect.right},${rect.bottom}]")
        if (node.childCount > 0) {
            sb.append(","children":[")
            for (i in 0 until node.childCount) {
                if (i > 0) sb.append(",")
                val child = node.getChild(i)
                if (child != null) sb.append(buildJsonNode(child, depth + 1))
                else sb.append("null")
            }
            sb.append("]")
        }
        sb.append("}")
        return sb.toString()
    }

}
