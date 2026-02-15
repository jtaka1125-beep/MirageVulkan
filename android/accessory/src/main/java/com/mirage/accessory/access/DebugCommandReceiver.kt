package com.mirage.accessory.access

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log

/**
 * Debug receiver for testing AccessibilityService gestures via ADB shell.
 * 
 * Usage:
 *   adb shell am broadcast -a com.mirage.accessory.DEBUG_CMD --es cmd tap --ei x 400 --ei y 634
 *   adb shell am broadcast -a com.mirage.accessory.DEBUG_CMD --es cmd swipe --ei x1 400 --ei y1 200 --ei x2 400 --ei y2 800 --ei dur 300
 *   adb shell am broadcast -a com.mirage.accessory.DEBUG_CMD --es cmd longpress --ei x 400 --ei y 634 --ei dur 1000
 *   adb shell am broadcast -a com.mirage.accessory.DEBUG_CMD --es cmd back
 */
class DebugCommandReceiver : BroadcastReceiver() {
    companion object {
        private const val TAG = "MirageDebugCmd"
    }

    override fun onReceive(context: Context?, intent: Intent?) {
        if (intent?.action != "com.mirage.accessory.DEBUG_CMD") return

        val a11y = MirageAccessibilityService.instance
        if (a11y == null) {
            Log.e(TAG, "AccessibilityService not running")
            return
        }

        val cmd = intent.getStringExtra("cmd") ?: return
        Log.i(TAG, "Debug command: $cmd")

        when (cmd) {
            "tap" -> {
                val x = intent.getIntExtra("x", 400)
                val y = intent.getIntExtra("y", 634)
                Log.i(TAG, "TAP ($x, $y)")
                a11y.tap(x.toFloat(), y.toFloat(), 0)
            }
            "swipe" -> {
                val x1 = intent.getIntExtra("x1", 400)
                val y1 = intent.getIntExtra("y1", 200)
                val x2 = intent.getIntExtra("x2", 400)
                val y2 = intent.getIntExtra("y2", 800)
                val dur = intent.getIntExtra("dur", 300)
                Log.i(TAG, "SWIPE ($x1,$y1)->($x2,$y2) ${dur}ms")
                a11y.swipe(x1.toFloat(), y1.toFloat(), x2.toFloat(), y2.toFloat(), dur, 0)
            }
            "longpress" -> {
                val x = intent.getIntExtra("x", 400)
                val y = intent.getIntExtra("y", 634)
                val dur = intent.getIntExtra("dur", 1000)
                Log.i(TAG, "LONGPRESS ($x, $y) ${dur}ms")
                a11y.longPress(x.toFloat(), y.toFloat(), dur, 0)
            }
            "back" -> {
                Log.i(TAG, "BACK")
                a11y.performBack(0)
            }
            "pinch" -> {
                val cx = intent.getIntExtra("cx", 400)
                val cy = intent.getIntExtra("cy", 634)
                val sd = intent.getIntExtra("sd", 300)
                val ed = intent.getIntExtra("ed", 100)
                val dur = intent.getIntExtra("dur", 500)
                Log.i(TAG, "PINCH center=($cx,$cy) $sd->$ed ${dur}ms")
                a11y.pinch(cx.toFloat(), cy.toFloat(), sd, ed, dur, 0, 0)
            }
        }
    }
}
