package com.mirage.android.svc

import android.app.Service
import android.content.Intent
import android.os.IBinder
import android.util.Log
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

class WatchdogService : Service() {
    companion object {
        private const val TAG = "MirageWatchdog"
    }

    private val exec = Executors.newSingleThreadScheduledExecutor()

    override fun onCreate() {
        super.onCreate()
        exec.scheduleAtFixedRate({
            // Scaffold: replace with heartbeat checks for Capture/Tx/Accessibility.
            Log.d(TAG, "tick")
        }, 2, 2, TimeUnit.SECONDS)
    }

    override fun onDestroy() {
        exec.shutdownNow()
        try {
            if (!exec.awaitTermination(1, TimeUnit.SECONDS)) {
                Log.w(TAG, "Executor did not terminate in time")
            }
        } catch (e: InterruptedException) {
            Log.w(TAG, "Interrupted while waiting for executor termination")
        }
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null
}
