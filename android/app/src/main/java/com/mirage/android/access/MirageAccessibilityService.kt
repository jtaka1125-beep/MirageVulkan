package com.mirage.android.access

import android.accessibilityservice.AccessibilityService
import android.accessibilityservice.GestureDescription
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.Path
import android.util.Log
import android.view.accessibility.AccessibilityEvent
import android.view.accessibility.AccessibilityNodeInfo
import androidx.localbroadcastmanager.content.LocalBroadcastManager
import com.mirage.android.core.Config
import com.mirage.android.svc.UdpSender
import com.mirage.android.usb.AccessoryIoService

class MirageAccessibilityService : AccessibilityService() {
    companion object {
        private const val TAG = "MirageA11y"

        @Volatile
        var instance: MirageAccessibilityService? = null
            private set
    }

    private val udpSender = UdpSender()

    private val commandReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent?.action != AccessoryIoService.ACTION_COMMAND) return

            val cmdType = intent.getIntExtra(AccessoryIoService.EXTRA_COMMAND_TYPE, -1)
            val seq = intent.getIntExtra(AccessoryIoService.EXTRA_SEQ, 0)
            val x = intent.getIntExtra(AccessoryIoService.EXTRA_X, 0)
            val y = intent.getIntExtra(AccessoryIoService.EXTRA_Y, 0)
            val keycode = intent.getIntExtra(AccessoryIoService.EXTRA_KEYCODE, 0)

            Log.d(TAG, "Received USB command: type=$cmdType seq=$seq x=$x y=$y")

            when (cmdType) {
                AccessoryIoService.CMD_TYPE_PING -> {
                    Log.i(TAG, "PING received seq=$seq")
                }
                AccessoryIoService.CMD_TYPE_TAP -> {
                    Log.i(TAG, "TAP x=$x y=$y seq=$seq")
                    tap(x.toFloat(), y.toFloat(), seq)
                }
                AccessoryIoService.CMD_TYPE_BACK -> {
                    Log.i(TAG, "BACK seq=$seq")
                    performBack(seq)
                }
                AccessoryIoService.CMD_TYPE_KEY -> {
                    Log.i(TAG, "KEY keycode=$keycode seq=$seq")
                }
            }
        }
    }

    override fun onServiceConnected() {
        super.onServiceConnected()
        instance = this

        udpSender.start()
        val filter = IntentFilter(AccessoryIoService.ACTION_COMMAND)
        LocalBroadcastManager.getInstance(this).registerReceiver(commandReceiver, filter)
        Log.i(TAG, "AccessibilityService connected and receiver registered")
    }

    override fun onAccessibilityEvent(event: AccessibilityEvent?) {
        if (event == null) return
        if (event.eventType == AccessibilityEvent.TYPE_WINDOW_STATE_CHANGED ||
            event.eventType == AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED) {
            Log.d(TAG, "event=${event.eventType} pkg=${event.packageName}")
        }
    }

    override fun onInterrupt() {}

    override fun onDestroy() {
        LocalBroadcastManager.getInstance(this).unregisterReceiver(commandReceiver)
        udpSender.stop()
        instance = null
        super.onDestroy()
    }

    fun tap(x: Float, y: Float, seq: Int = 0) {
        val p = Path().apply { moveTo(x, y) }
        val stroke = GestureDescription.StrokeDescription(p, 0, 50)
        val gd = GestureDescription.Builder().addStroke(stroke).build()
        val tStart = System.currentTimeMillis()
        dispatchGesture(gd, object : GestureResultCallback() {
            override fun onCompleted(gestureDescription: GestureDescription?) {
                val tEnd = System.currentTimeMillis()
                Log.d(TAG, "Tap gesture completed at ($x, $y) seq=$seq")
                // Send UDP notification
                val json = """{"slot":${Config.DEFAULT_SLOT},"seq":$seq,"x":${x.toInt()},"y":${y.toInt()},"ok":true,"latency_ms":${tEnd - tStart}}"""
                udpSender.trySendLine(Config.TAG_TAP_EXEC, json)
            }
            override fun onCancelled(gestureDescription: GestureDescription?) {
                Log.w(TAG, "Tap gesture cancelled at ($x, $y) seq=$seq")
                val json = """{"slot":${Config.DEFAULT_SLOT},"seq":$seq,"x":${x.toInt()},"y":${y.toInt()},"ok":false}"""
                udpSender.trySendLine(Config.TAG_TAP_EXEC, json)
            }
        }, null)
    }

    fun performBack(seq: Int = 0) {
        val result = performGlobalAction(GLOBAL_ACTION_BACK)
        Log.d(TAG, "BACK action result=$result seq=$seq")
        val json = """{"slot":${Config.DEFAULT_SLOT},"seq":$seq,"ok":$result}"""
        udpSender.trySendLine(Config.TAG_BACK_EXEC, json)
    }

    private fun dumpNode(root: AccessibilityNodeInfo?, depth: Int = 0) {
        if (root == null || depth > 30) return
    }
}
