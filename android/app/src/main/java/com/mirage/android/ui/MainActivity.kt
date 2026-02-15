package com.mirage.android.ui

import android.Manifest
import android.app.Activity
import android.app.ActivityManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.graphics.Color
import android.hardware.usb.UsbAccessory
import android.hardware.usb.UsbManager
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.util.Log
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import com.mirage.android.R
import com.mirage.android.access.MirageAccessibilityService
import com.mirage.android.capture.ScreenCaptureService
import com.mirage.android.svc.CaptureService
import com.mirage.android.svc.TxService
import com.mirage.android.svc.WatchdogService
import com.mirage.android.audio.AudioCaptureService
import com.mirage.android.usb.AccessoryIoService

class MainActivity : AppCompatActivity() {
    private lateinit var status: TextView
    private lateinit var mirrorStatus: TextView
    private lateinit var audioStatus: TextView
    private lateinit var editMirrorHost: EditText
    private lateinit var editMirrorPort: EditText
    private lateinit var mpm: MediaProjectionManager

    private lateinit var setupPanel: LinearLayout
    private lateinit var gate1Icon: TextView
    private lateinit var gate2Icon: TextView
    private lateinit var gate3Icon: TextView
    private lateinit var gate4Icon: TextView
    private lateinit var gate1Row: LinearLayout
    private lateinit var gate2Row: LinearLayout
    private lateinit var nextActionButton: Button

    private var gate1 = false
    private var gate2 = false
    private var gate3 = false
    private var gate4 = false
    private var currentAccessory: UsbAccessory? = null

    private val updateHandler = Handler(Looper.getMainLooper())
    private val updateRunnable = object : Runnable {
        override fun run() {
            updateGateStatus()
            updateGateIcons()
            updateHandler.postDelayed(this, 1000)
        }
    }

    companion object {
        private const val TAG = "MirageMain"
        private const val REQUEST_RECORD_AUDIO_PERMISSION = 200
        private const val ACTION_USB_PERMISSION = "com.mirage.android.USB_PERMISSION"
    }

    private var pendingAccessoryPermission = false

    // Track which projection request is pending
    private var pendingProjectionType = ""

    // ActivityResultLauncher for MediaProjection (Android 14+ compatible)
    private lateinit var projectionLauncher: ActivityResultLauncher<Intent>

    private val usbPermissionReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent?.action != ACTION_USB_PERMISSION) return
            synchronized(this) {
                val accessory = intent.getParcelableExtra<UsbAccessory>(UsbManager.EXTRA_ACCESSORY)
                val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                Log.i(TAG, "USB permission result: granted=$granted accessory=$accessory")
                pendingAccessoryPermission = false
                if (granted && accessory != null) {
                    startAccessoryService(accessory)
                }
            }
        }
    }

    private var autoMirrorHost: String? = null
    private var autoMirrorPort: Int = 50000

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        status = findViewById(R.id.statusText)
        mirrorStatus = findViewById(R.id.mirrorStatusText)
        editMirrorHost = findViewById(R.id.editMirrorHost)
        editMirrorPort = findViewById(R.id.editMirrorPort)
        mpm = getSystemService(MediaProjectionManager::class.java)

        setupPanel = findViewById(R.id.setupPanel)
        gate1Icon = findViewById(R.id.gate1Icon)
        gate2Icon = findViewById(R.id.gate2Icon)
        gate3Icon = findViewById(R.id.gate3Icon)
        gate4Icon = findViewById(R.id.gate4Icon)
        gate1Row = findViewById(R.id.gate1Row)
        gate2Row = findViewById(R.id.gate2Row)
        nextActionButton = findViewById(R.id.nextActionButton)

        audioStatus = findViewById(R.id.audioStatusText)

        // Register ActivityResultLauncher BEFORE onCreate finishes
        projectionLauncher = registerForActivityResult(
            ActivityResultContracts.StartActivityForResult()
        ) { result ->
            handleProjectionResult(result.resultCode, result.data)
        }

        gate1Row.setOnClickListener { openNotificationSettings() }
        gate2Row.setOnClickListener { openAccessibilitySettings() }
        nextActionButton.setOnClickListener { handleNextAction() }

        findViewById<Button>(R.id.btnStart).setOnClickListener {
            pendingProjectionType = "projection"
            launchProjectionRequest()
        }
        findViewById<Button>(R.id.btnStop).setOnClickListener {
            stopService(Intent(this, CaptureService::class.java))
            stopService(Intent(this, TxService::class.java))
            stopService(Intent(this, WatchdogService::class.java))
            stopService(Intent(this, AccessoryIoService::class.java))
            status.text = "Stopped"
        }
        findViewById<Button>(R.id.btnMirrorStart).setOnClickListener {
            pendingProjectionType = "mirror"
            launchProjectionRequest()
        }
        findViewById<Button>(R.id.btnTcpMirrorStart).setOnClickListener {
            pendingProjectionType = "tcp_mirror"
            launchProjectionRequest()
        }
        findViewById<Button>(R.id.btnMirrorStop).setOnClickListener {
            stopService(Intent(this, ScreenCaptureService::class.java))
            mirrorStatus.text = "Mirror: Stopped"
        }

        // Audio buttons
        findViewById<Button>(R.id.btnAudioStart).setOnClickListener {
            if (!gate4) {
                audioStatus.text = "Audio: USB not connected"
                return@setOnClickListener
            }
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
                audioStatus.text = "Audio: Requires Android 10+"
                Toast.makeText(this, "Audio capture requires Android 10 or higher", Toast.LENGTH_LONG).show()
                return@setOnClickListener
            }
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
                    != PackageManager.PERMISSION_GRANTED) {
                ActivityCompat.requestPermissions(
                    this,
                    arrayOf(Manifest.permission.RECORD_AUDIO),
                    REQUEST_RECORD_AUDIO_PERMISSION
                )
                return@setOnClickListener
            }
            startAudioCapture()
        }
        findViewById<Button>(R.id.btnAudioStop).setOnClickListener {
            stopService(Intent(this, AudioCaptureService::class.java))
            audioStatus.text = "Audio: Stopped"
        }

        // Register USB permission receiver
        val filter = IntentFilter(ACTION_USB_PERMISSION)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(usbPermissionReceiver, filter, RECEIVER_EXPORTED)
        } else {
            registerReceiver(usbPermissionReceiver, filter)
        }

        updateGateStatus()
        updateGateIcons()
        checkAutoMirror(intent)

        // Handle USB_ACCESSORY_ATTACHED intent on initial launch
        handleAccessoryIntent(intent)
    }

    /**
     * Launch MediaProjection permission request using ActivityResultLauncher.
     * This is compatible with Android 14+ which deprecated startActivityForResult
     * for MediaProjection and may instantly cancel the dialog when using the old API.
     */
    private fun launchProjectionRequest() {
        Log.i(TAG, "Launching projection request: type=$pendingProjectionType")
        try {
            projectionLauncher.launch(mpm.createScreenCaptureIntent())
        } catch (e: Exception) {
            Log.e(TAG, "Failed to launch projection request", e)
            when (pendingProjectionType) {
                "mirror", "tcp_mirror" -> mirrorStatus.text = "Mirror: Launch failed"
                "audio" -> audioStatus.text = "Audio: Launch failed"
                else -> status.text = "MediaProjection launch failed"
            }
        }
    }

    /**
     * Handle the result from MediaProjection permission dialog.
     * Dispatches to the appropriate handler based on pendingProjectionType.
     */
    private fun handleProjectionResult(resultCode: Int, data: Intent?) {
        Log.i(TAG, "Projection result: type=$pendingProjectionType resultCode=$resultCode data=${data != null}")

        if (resultCode != Activity.RESULT_OK || data == null) {
            when (pendingProjectionType) {
                "mirror", "tcp_mirror" -> mirrorStatus.text = "Mirror: Permission denied"
                "audio" -> audioStatus.text = "Audio: Permission denied"
                else -> status.text = "MediaProjection denied"
            }
            pendingProjectionType = ""
            return
        }

        when (pendingProjectionType) {
            "projection" -> {
                startService(Intent(this, TxService::class.java))
                startService(Intent(this, WatchdogService::class.java))
                val i = Intent(this, CaptureService::class.java).apply {
                    putExtra(CaptureService.EXTRA_RESULT_CODE, resultCode)
                    putExtra(CaptureService.EXTRA_RESULT_DATA, data)
                }
                startForegroundService(i)
                status.text = "Started (scaffold). Enable AccessibilityService in Settings."
            }
            "mirror" -> {
                val host = editMirrorHost.text.toString().ifBlank { "192.168.0.2" }
                val port = editMirrorPort.text.toString().toIntOrNull() ?: 50000
                val usbManager = getSystemService(UsbManager::class.java)
                val hasUsb = usbManager.accessoryList?.isNotEmpty() == true
                val mirrorMode = if (hasUsb) {
                    ScreenCaptureService.MIRROR_MODE_USB
                } else {
                    ScreenCaptureService.MIRROR_MODE_UDP
                }
                val i = Intent(this, ScreenCaptureService::class.java).apply {
                    putExtra(ScreenCaptureService.EXTRA_RESULT_CODE, resultCode)
                    putExtra(ScreenCaptureService.EXTRA_RESULT_DATA, data)
                    putExtra(ScreenCaptureService.EXTRA_HOST, host)
                    putExtra(ScreenCaptureService.EXTRA_PORT, port)
                    putExtra(ScreenCaptureService.EXTRA_MIRROR_MODE, mirrorMode)
                }
                startForegroundService(i)
                val modeStr = if (hasUsb) "USB" else "UDP"
                mirrorStatus.text = "Mirror: $modeStr to $host:$port"
                Log.i(TAG, "Mirror started ($modeStr) to $host:$port")
            }
            "tcp_mirror" -> {
                val i = Intent(this, ScreenCaptureService::class.java).apply {
                    putExtra(ScreenCaptureService.EXTRA_RESULT_CODE, resultCode)
                    putExtra(ScreenCaptureService.EXTRA_RESULT_DATA, data)
                    putExtra(ScreenCaptureService.EXTRA_MIRROR_MODE, ScreenCaptureService.MIRROR_MODE_TCP)
                }
                startForegroundService(i)
                mirrorStatus.text = "Mirror: TCP (ADB forward) on port 50100"
                Log.i(TAG, "TCP Mirror started on localhost:50100")
            }
            "audio" -> {
                val i = Intent(this, AudioCaptureService::class.java).apply {
                    putExtra(AudioCaptureService.EXTRA_RESULT_CODE, resultCode)
                    putExtra(AudioCaptureService.EXTRA_RESULT_DATA, data)
                }
                startForegroundService(i)
                audioStatus.text = "Audio: Streaming via USB"
                Log.i(TAG, "Audio capture started")
            }
        }
        pendingProjectionType = ""
    }

    /**
     * Handle USB accessory attached intent.
     */
    private fun handleAccessoryIntent(intent: Intent?) {
        if (intent == null) return
        if (intent.action != UsbManager.ACTION_USB_ACCESSORY_ATTACHED) return

        val accessory = intent.getParcelableExtra<UsbAccessory>(UsbManager.EXTRA_ACCESSORY)
        if (accessory == null) {
            Log.w(TAG, "USB_ACCESSORY_ATTACHED but no accessory in intent")
            return
        }

        Log.i(TAG, "USB Accessory attached: ${accessory.manufacturer}/${accessory.model}")

        if (isServiceRunning(AccessoryIoService::class.java)) {
            Log.i(TAG, "AccessoryIoService already running, skipping")
            return
        }

        val serviceIntent = Intent(this, AccessoryIoService::class.java).apply {
            putExtra(UsbManager.EXTRA_ACCESSORY, accessory)
        }
        startForegroundService(serviceIntent)
        Log.i(TAG, "AccessoryIoService started via USB_ACCESSORY_ATTACHED")
        status.text = "USB Accessory connected"

        currentAccessory = accessory
        gate3 = true
        updateGateIcons()
    }

    private fun updateGateStatus() {
        gate1 = NotificationManagerCompat.from(this).areNotificationsEnabled()
        gate2 = isAccessibilityServiceEnabled()
        val usbManager = getSystemService(UsbManager::class.java)
        val accessories = usbManager.accessoryList
        currentAccessory = if (accessories != null && accessories.isNotEmpty()) accessories[0] else null
        gate3 = currentAccessory != null
        gate4 = isServiceRunning(AccessoryIoService::class.java)

        if (gate3 && !gate4 && currentAccessory != null && !pendingAccessoryPermission) {
            val hasPermission = usbManager.hasPermission(currentAccessory)
            if (hasPermission) {
                Log.i(TAG, "Auto-starting AccessoryIoService for detected accessory")
                startAccessoryService(currentAccessory!!)
            } else {
                Log.i(TAG, "Requesting USB permission for accessory")
                pendingAccessoryPermission = true
                val permissionIntent = PendingIntent.getBroadcast(
                    this, 0,
                    Intent(ACTION_USB_PERMISSION).apply {
                        setPackage(packageName)
                    },
                    PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE
                )
                usbManager.requestPermission(currentAccessory, permissionIntent)
            }
        }

        if (gate1 && gate2 && gate3 && gate4) {
            setupPanel.visibility = View.GONE
        } else {
            setupPanel.visibility = View.VISIBLE
        }
    }

    private fun updateGateIcons() {
        if (gate1) {
            gate1Icon.text = "OK"
            gate1Icon.setTextColor(Color.parseColor("#008000"))
        } else {
            gate1Icon.text = "NG"
            gate1Icon.setTextColor(Color.RED)
        }
        if (gate2) {
            gate2Icon.text = "OK"
            gate2Icon.setTextColor(Color.parseColor("#008000"))
        } else {
            gate2Icon.text = "NG"
            gate2Icon.setTextColor(Color.RED)
        }
        when {
            gate3 -> {
                gate3Icon.text = "OK"
                gate3Icon.setTextColor(Color.parseColor("#008000"))
            }
            !gate1 || !gate2 -> {
                gate3Icon.text = "--"
                gate3Icon.setTextColor(Color.GRAY)
            }
            else -> {
                gate3Icon.text = ".."
                gate3Icon.setTextColor(Color.parseColor("#FFA500"))
            }
        }
        when {
            gate4 -> {
                gate4Icon.text = "OK"
                gate4Icon.setTextColor(Color.parseColor("#008000"))
            }
            !gate3 -> {
                gate4Icon.text = "--"
                gate4Icon.setTextColor(Color.GRAY)
            }
            else -> {
                gate4Icon.text = ".."
                gate4Icon.setTextColor(Color.parseColor("#FFA500"))
            }
        }
        nextActionButton.text = getNextActionText()
    }

    private fun getNextActionText(): String = when {
        !gate1 -> "通知権限を許可する"
        !gate2 -> "Accessibilityを有効化する"
        !gate3 -> "USBケーブルを接続してください"
        !gate4 -> "サービス起動中..."
        else -> "セットアップ完了！"
    }

    private fun handleNextAction() {
        when {
            !gate1 -> openNotificationSettings()
            !gate2 -> openAccessibilitySettings()
            !gate3 -> { }
            !gate4 -> { }
        }
    }

    private fun openNotificationSettings() {
        val intent = Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS).apply {
            putExtra(Settings.EXTRA_APP_PACKAGE, packageName)
        }
        startActivity(intent)
    }

    private fun openAccessibilitySettings() {
        startActivity(Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS))
    }

    private fun isAccessibilityServiceEnabled(): Boolean {
        val serviceName = "${packageName}/${MirageAccessibilityService::class.java.canonicalName}"
        val enabledServices = Settings.Secure.getString(
            contentResolver,
            Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES
        ) ?: return false
        return enabledServices.contains(serviceName)
    }

    @Suppress("DEPRECATION")
    private fun isServiceRunning(serviceClass: Class<*>): Boolean {
        val manager = getSystemService(ACTIVITY_SERVICE) as ActivityManager
        for (service in manager.getRunningServices(Int.MAX_VALUE)) {
            if (serviceClass.name == service.service.className) {
                return true
            }
        }
        return false
    }

    private fun checkAutoMirror(intent: Intent) {
        val autoMirror = intent.getBooleanExtra("auto_mirror", false)
        if (autoMirror) {
            autoMirrorHost = intent.getStringExtra("mirror_host")
            autoMirrorPort = intent.getIntExtra("mirror_port", 50000)
            if (autoMirrorHost != null) {
                Log.i(TAG, "Auto mirror requested: $autoMirrorHost:$autoMirrorPort")
                editMirrorHost.setText(autoMirrorHost)
                editMirrorPort.setText(autoMirrorPort.toString())
                pendingProjectionType = "mirror"
                launchProjectionRequest()
            }
        }
    }

    override fun onNewIntent(intent: Intent?) {
        super.onNewIntent(intent)
        setIntent(intent)
        intent?.let {
            checkAutoMirror(it)
            handleAccessoryIntent(it)
        }
    }

    override fun onResume() {
        super.onResume()
        updateHandler.post(updateRunnable)
    }

    override fun onPause() {
        super.onPause()
        updateHandler.removeCallbacks(updateRunnable)
    }

    override fun onDestroy() {
        super.onDestroy()
        try {
            unregisterReceiver(usbPermissionReceiver)
        } catch (e: Exception) {
            Log.w(TAG, "Failed to unregister USB receiver", e)
        }
    }

    private fun startAccessoryService(accessory: UsbAccessory) {
        val serviceIntent = Intent(this, AccessoryIoService::class.java).apply {
            putExtra(UsbManager.EXTRA_ACCESSORY, accessory)
        }
        try {
            startForegroundService(serviceIntent)
            Log.i(TAG, "AccessoryIoService started")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to start AccessoryIoService", e)
        }
    }

    private fun startAudioCapture() {
        pendingProjectionType = "audio"
        launchProjectionRequest()
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        when (requestCode) {
            REQUEST_RECORD_AUDIO_PERMISSION -> {
                if (grantResults.isNotEmpty() && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
                    startAudioCapture()
                } else {
                    audioStatus.text = "Audio: Permission denied"
                    Toast.makeText(this, "RECORD_AUDIO permission required for audio capture", Toast.LENGTH_LONG).show()
                }
            }
        }
    }
}
