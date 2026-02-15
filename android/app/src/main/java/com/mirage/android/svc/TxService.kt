package com.mirage.android.svc

import android.app.Service
import android.content.Intent
import android.os.IBinder
import android.util.Log
import com.mirage.android.core.Config
import com.mirage.android.core.VidMeta
import java.util.concurrent.atomic.AtomicLong

class TxService : Service() {
    private val sender = UdpSender()
    private val seq = AtomicLong(0)

    override fun onCreate() {
        super.onCreate()
        sender.start()
        Log.i("MirageTx", "TxService started")
    }

    override fun onDestroy() {
        sender.stop()
        Log.i("MirageTx", "TxService stopped")
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    fun publishVidMeta(bytes: Int, capMonoNs: Long, capWallMs: Long, keyframe: Boolean, fpsHint: Int) {
        val meta = VidMeta(
            slot = Config.DEFAULT_SLOT,
            seq = seq.incrementAndGet(),
            codec = "raw", // scaffold: replace with "h264" when MediaCodec wired
            bytes = bytes,
            capMonoNs = capMonoNs,
            capWallMs = capWallMs,
            keyframe = keyframe,
            path = "usb", // or "wifi"
            fpsHint = fpsHint
        )
        sender.trySendLine(Config.TAG_VIDMETA, meta.toJson())
    }

    fun publishUiTree(json: String) {
        sender.trySendLine(Config.TAG_UITREE, json)
    }

    fun publishLog(json: String) {
        sender.trySendLine(Config.TAG_LOG, json)
    }
}
