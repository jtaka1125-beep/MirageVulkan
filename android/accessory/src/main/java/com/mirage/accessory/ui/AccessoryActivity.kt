package com.mirage.accessory.ui

import android.app.ActivityManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.Color
import android.hardware.usb.UsbAccessory
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.util.Log
import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.NotificationManagerCompat
import com.mirage.accessory.R
import com.mirage.accessory.access.MirageAccessibilityService
import com.mirage.accessory.usb.AccessoryIoService

class AccessoryActivity : AppCompatActivity() {
    private lateinit var statusText: TextView
    private lateinit var gate1Icon: TextView  // Notification
    private lateinit var gate2Icon: TextView  // Accessibility
    private lateinit var gate3Icon: TextView  // USB connected
    private lateinit var gate4Icon: TextView  // Service running

    private var currentAccessory: UsbAccessory? = null
    private var pendingPermission = false

    companion object {
        private const val TAG = "MirageAccessory"
        private const val ACTION_USB_PERMISSION = "com.mirage.accessory.USB_PERMISSION"
    }

    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent?.action != ACTION_USB_PERMISSION) return
            synchronized(this) {
                val accessory = intent.getParcelableExtra<UsbAccessory>(UsbManager.EXTRA_ACCESSORY)
                val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                Log.i(TAG, "USB permission: granted=$granted")
                pendingPermission = false
                if (granted && accessory != null) startAccessoryService(accessory)
            }
        }
    }

    private val updateHandler = Handler(Looper.getMainLooper())
    private val updateRunnable = object : Runnable {
        override fun run() {
            updateStatus()
            updateHandler.postDelayed(this, 1000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_accessory)

        statusText = findViewById(R.id.statusText)
        gate1Icon = findViewById(R.id.gate1Icon)
        gate2Icon = findViewById(R.id.gate2Icon)
        gate3Icon = findViewById(R.id.gate3Icon)
        gate4Icon = findViewById(R.id.gate4Icon)

        findViewById<Button>(R.id.btnNotification).setOnClickListener {
            startActivity(Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS).apply {
                putExtra(Settings.EXTRA_APP_PACKAGE, packageName)
            })
        }
        findViewById<Button>(R.id.btnAccessibility).setOnClickListener {
            startActivity(Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS))
        }

        val filter = IntentFilter(ACTION_USB_PERMISSION)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(usbReceiver, filter, RECEIVER_EXPORTED)
        } else {
            registerReceiver(usbReceiver, filter)
        }

        handleAccessoryIntent(intent)
    }

    override fun onNewIntent(intent: Intent?) {
        super.onNewIntent(intent)
        setIntent(intent)
        intent?.let { handleAccessoryIntent(it) }
    }

    private fun handleAccessoryIntent(intent: Intent?) {
        if (intent?.action != UsbManager.ACTION_USB_ACCESSORY_ATTACHED) return
        val accessory = intent.getParcelableExtra<UsbAccessory>(UsbManager.EXTRA_ACCESSORY) ?: return
        Log.i(TAG, "Accessory attached: ${accessory.manufacturer}/${accessory.model}")
        if (!isServiceRunning(AccessoryIoService::class.java)) {
            startAccessoryService(accessory)
        }
        currentAccessory = accessory
    }

    private fun updateStatus() {
        val g1 = NotificationManagerCompat.from(this).areNotificationsEnabled()
        val g2 = isAccessibilityEnabled()
        val usbMgr = getSystemService(UsbManager::class.java)
        val accessories = usbMgr.accessoryList
        currentAccessory = accessories?.firstOrNull()
        val g3 = currentAccessory != null
        val g4 = isServiceRunning(AccessoryIoService::class.java)

        setGate(gate1Icon, g1)
        setGate(gate2Icon, g2)
        setGate(gate3Icon, g3)
        setGate(gate4Icon, g4)

        // Auto-start if connected but service not running
        if (g3 && !g4 && !pendingPermission && currentAccessory != null) {
            if (usbMgr.hasPermission(currentAccessory)) {
                startAccessoryService(currentAccessory!!)
            } else {
                pendingPermission = true
                val pi = PendingIntent.getBroadcast(this, 0,
                    Intent(ACTION_USB_PERMISSION).apply { setPackage(packageName) },
                    PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
                usbMgr.requestPermission(currentAccessory, pi)
            }
        }

        statusText.text = when {
            g4 -> "AOA Active"
            g3 -> "Connecting..."
            else -> "Waiting for USB"
        }
    }

    private fun setGate(tv: TextView, ok: Boolean) {
        tv.text = if (ok) "OK" else "NG"
        tv.setTextColor(if (ok) Color.parseColor("#008000") else Color.RED)
    }

    private fun isAccessibilityEnabled(): Boolean {
        val svc = "${packageName}/${MirageAccessibilityService::class.java.canonicalName}"
        val enabled = Settings.Secure.getString(contentResolver, Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES) ?: return false
        return enabled.contains(svc)
    }

    @Suppress("DEPRECATION")
    private fun isServiceRunning(cls: Class<*>): Boolean {
        val mgr = getSystemService(ACTIVITY_SERVICE) as ActivityManager
        return mgr.getRunningServices(Int.MAX_VALUE).any { it.service.className == cls.name }
    }

    private fun startAccessoryService(accessory: UsbAccessory) {
        try {
            startForegroundService(Intent(this, AccessoryIoService::class.java).apply {
                putExtra(UsbManager.EXTRA_ACCESSORY, accessory)
            })
            Log.i(TAG, "AccessoryIoService started")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to start AccessoryIoService", e)
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
        try { unregisterReceiver(usbReceiver) } catch (_: Exception) {}
    }
}
