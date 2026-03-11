package com.mirage.capture.usb

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.hardware.usb.UsbAccessory
import android.hardware.usb.UsbManager
import android.util.Log
import com.mirage.capture.util.RouteTrace

/**
 * BroadcastReceiver to handle USB_ACCESSORY_ATTACHED intents.
 * This allows AccessoryIoService to start automatically without requiring
 * the AccessoryActivity to be in the foreground.
 */
class UsbAccessoryReceiver : BroadcastReceiver() {

    companion object {
        private const val TAG = "MirageUsbReceiver"
    }

    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != UsbManager.ACTION_USB_ACCESSORY_ATTACHED) return

        val accessory = intent.getParcelableExtra<UsbAccessory>(UsbManager.EXTRA_ACCESSORY)
        if (accessory == null) {
            Log.w(TAG, "USB_ACCESSORY_ATTACHED but no accessory in intent")
            return
        }

        Log.i(TAG, "USB_ACCESSORY_ATTACHED: ${accessory.manufacturer}/${accessory.model}")
        RouteTrace.append(context, "UsbAccessoryReceiver: attached ${accessory.manufacturer}/${accessory.model}")

        // Check if service is already running
        if (AccessoryIoService.instance != null) {
            Log.i(TAG, "AccessoryIoService already running, ignoring")
            return
        }

        // Start AccessoryIoService with the accessory
        try {
            val serviceIntent = Intent(context, AccessoryIoService::class.java).apply {
                putExtra(UsbManager.EXTRA_ACCESSORY, accessory)
            }
            context.startForegroundService(serviceIntent)
            Log.i(TAG, "Started AccessoryIoService via BroadcastReceiver")
            RouteTrace.append(context, "UsbAccessoryReceiver: started AccessoryIoService")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to start AccessoryIoService", e)
            RouteTrace.append(context, "UsbAccessoryReceiver: failed to start service: ${e.message}")
        }
    }
}
