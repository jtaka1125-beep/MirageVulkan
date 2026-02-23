package com.mirage.capture.ui

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import com.mirage.capture.R
import com.mirage.capture.capture.ScreenCaptureService
import com.mirage.capture.audio.AudioCaptureService

class CaptureActivity : AppCompatActivity() {
    private lateinit var statusText: TextView
    private lateinit var editHost: EditText
    private lateinit var editPort: EditText
    private lateinit var mpm: android.media.projection.MediaProjectionManager

    private var pendingMode: String? = null

    companion object {
        private const val TAG = "MirageCapture"
        private const val MODE_UDP = "udp"
        private const val MODE_TCP = "tcp"
        private const val MODE_AUDIO = "audio"
        private const val KEY_PENDING_MODE = "pending_mode"
    }

    // --- ActivityResultLaunchers ---

    private val projectionLauncher: ActivityResultLauncher<Intent> =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            Log.i(TAG, "projectionLauncher callback: resultCode=${result.resultCode} data=${result.data != null} pendingMode=$pendingMode")
            if (result.resultCode != Activity.RESULT_OK || result.data == null) {
                statusText.text = "Permission denied"
                return@registerForActivityResult
            }
            when (pendingMode) {
                MODE_UDP -> startUdpMirror(result.resultCode, result.data!!)
                MODE_TCP -> startTcpMirror(result.resultCode, result.data!!)
                MODE_AUDIO -> startAudioCapture(result.resultCode, result.data!!)
                else -> Log.e(TAG, "pendingMode is null, cannot dispatch result")
            }
            pendingMode = null
        }

    private val recordAudioPermLauncher: ActivityResultLauncher<String> =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (granted) requestProjection(MODE_AUDIO)
            else statusText.text = "RECORD_AUDIO denied"
        }

    private val notificationPermLauncher: ActivityResultLauncher<String> =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            Log.i(TAG, "POST_NOTIFICATIONS: $granted")
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_capture)

        // Restore pending mode after activity recreation
        pendingMode = savedInstanceState?.getString(KEY_PENDING_MODE)
        Log.i(TAG, "onCreate: restored pendingMode=$pendingMode")

        statusText = findViewById(R.id.statusText)
        editHost = findViewById(R.id.editHost)
        editPort = findViewById(R.id.editPort)
        mpm = getSystemService(android.media.projection.MediaProjectionManager::class.java)

        // POST_NOTIFICATIONS on Android 13+
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
                    != PackageManager.PERMISSION_GRANTED) {
                notificationPermLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
            }
        }

        // Debug buttons (kept for manual control)
        findViewById<Button>(R.id.btnUdpStart).setOnClickListener {
            val mode = intent.getStringExtra("mirror_mode") ?: MODE_UDP
            requestProjection(mode)
        }
        findViewById<Button>(R.id.btnTcpStart).setOnClickListener { requestProjection(MODE_TCP) }
        findViewById<Button>(R.id.btnStop).setOnClickListener {
            stopService(Intent(this, ScreenCaptureService::class.java))
            statusText.text = "Stopped"
        }
        findViewById<Button>(R.id.btnAudioStart).setOnClickListener {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
                    != PackageManager.PERMISSION_GRANTED) {
                recordAudioPermLauncher.launch(Manifest.permission.RECORD_AUDIO)
                return@setOnClickListener
            }
            requestProjection(MODE_AUDIO)
        }
        findViewById<Button>(R.id.btnAudioStop).setOnClickListener {
            stopService(Intent(this, AudioCaptureService::class.java))
            statusText.text = "Audio stopped"
        }

        // === AUTO-START LOGIC ===
        // Priority: 1) Intent with auto_mirror  2) Auto-start TCP if service not running
        if (intent.getBooleanExtra("auto_mirror", false)) {
            handleAutoStart(intent)
        } else if (ScreenCaptureService.instance == null) {
            // No explicit intent, but service isn't running → auto-start TCP
            Log.i(TAG, "No auto_mirror intent, auto-starting TCP mirror (service not running)")
            statusText.text = "Auto-starting TCP mirror..."
            requestProjection(MODE_TCP)
        } else {
            // Service already running, just show status
            statusText.text = "Capture running (${ScreenCaptureService.instance?.mirrorMode ?: "unknown"} mode)"
            Log.i(TAG, "Service already running, no action needed")
        }
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putString(KEY_PENDING_MODE, pendingMode)
        Log.i(TAG, "onSaveInstanceState: pendingMode=$pendingMode")
    }

    override fun onNewIntent(intent: Intent?) {
        super.onNewIntent(intent)
        setIntent(intent)
        intent?.let { handleAutoStart(it) }
    }

    private fun handleAutoStart(intent: Intent) {
        val autoMirror = intent.getBooleanExtra("auto_mirror", false)
        if (autoMirror) {
            val mode = intent.getStringExtra("mirror_mode") ?: MODE_TCP  // Default to TCP
            val host = intent.getStringExtra("mirror_host") ?: ""
            val port = intent.getIntExtra("mirror_port", 50100)

            if (mode == MODE_UDP && host.isEmpty()) {
                Log.e(TAG, "Auto mirror: UDP mode requires mirror_host")
                return
            }
            if (host.isNotEmpty()) editHost.setText(host)
            editPort.setText(port.toString())
            Log.i(TAG, "Auto mirror: mode=$mode host=$host port=$port")

            // If service already running, skip re-requesting projection
            if (ScreenCaptureService.instance != null) {
                statusText.text = "Capture already running"
                Log.i(TAG, "Auto mirror: service already running, skipping")
                return
            }
            requestProjection(mode)
        }
    }

    private fun requestProjection(mode: String) {
        pendingMode = mode
        Log.i(TAG, "requestProjection: mode=$mode")
        projectionLauncher.launch(mpm.createScreenCaptureIntent())
    }

    private fun startUdpMirror(resultCode: Int, data: Intent) {
        val host = editHost.text.toString().ifBlank { "192.168.0.2" }
        val port = editPort.text.toString().toIntOrNull() ?: 50000
        val i = Intent(this, ScreenCaptureService::class.java).apply {
            putExtra(ScreenCaptureService.EXTRA_RESULT_CODE, resultCode)
            putExtra(ScreenCaptureService.EXTRA_RESULT_DATA, data)
            putExtra(ScreenCaptureService.EXTRA_HOST, host)
            putExtra(ScreenCaptureService.EXTRA_PORT, port)
            putExtra(ScreenCaptureService.EXTRA_MIRROR_MODE, ScreenCaptureService.MIRROR_MODE_UDP)
        }
        Log.i(TAG, "startForegroundService: UDP → $host:$port")
        startForegroundService(i)
        statusText.text = "UDP → $host:$port"
    }

    private fun startTcpMirror(resultCode: Int, data: Intent) {
        val i = Intent(this, ScreenCaptureService::class.java).apply {
            putExtra(ScreenCaptureService.EXTRA_RESULT_CODE, resultCode)
            putExtra(ScreenCaptureService.EXTRA_RESULT_DATA, data)
            putExtra(ScreenCaptureService.EXTRA_MIRROR_MODE, ScreenCaptureService.MIRROR_MODE_TCP)
        }
        Log.i(TAG, "startForegroundService: TCP")
        startForegroundService(i)
        statusText.text = "TCP on :50100"
    }

    private fun startAudioCapture(resultCode: Int, data: Intent) {
        val i = Intent(this, AudioCaptureService::class.java).apply {
            putExtra(AudioCaptureService.EXTRA_RESULT_CODE, resultCode)
            putExtra(AudioCaptureService.EXTRA_RESULT_DATA, data)
            putExtra(AudioCaptureService.EXTRA_AUDIO_MODE, AudioCaptureService.AUDIO_MODE_TCP)
        }
        Log.i(TAG, "startForegroundService: Audio TCP")
        startForegroundService(i)
        statusText.text = "Audio streaming (TCP)"
    }
}
