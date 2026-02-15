package com.mirage.android.capture

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.media.projection.MediaProjectionManager
import android.util.Log

/**
 * Helper object to request MediaProjection permission and start capture service.
 *
 * Usage in Activity:
 * 1. Call CaptureController.requestPermission(activity) to show system dialog
 * 2. In onActivityResult, call CaptureController.onPermissionResult(...)
 * 3. If granted, call CaptureController.startCapture(...)
 */
object CaptureController {

    private const val TAG = "MirageCaptureCtrl"
    const val REQUEST_CODE = 5001

    /**
     * Request MediaProjection permission. Shows system dialog.
     */
    fun requestPermission(activity: Activity) {
        val mgr = activity.getSystemService(Context.MEDIA_PROJECTION_SERVICE)
            as MediaProjectionManager
        activity.startActivityForResult(mgr.createScreenCaptureIntent(), REQUEST_CODE)
        Log.i(TAG, "Permission requested")
    }

    /**
     * Handle onActivityResult. Returns true if permission was granted.
     */
    fun onPermissionResult(requestCode: Int, resultCode: Int): Boolean {
        if (requestCode != REQUEST_CODE) return false
        val granted = resultCode == Activity.RESULT_OK
        Log.i(TAG, "Permission result: granted=$granted")
        return granted
    }

    /**
     * Start the capture service with granted permission.
     *
     * @param activity Activity context
     * @param resultCode Result code from onActivityResult
     * @param data Intent data from onActivityResult
     * @param host UDP target host IP
     * @param port UDP target port
     */
    fun startCapture(
        activity: Activity,
        resultCode: Int,
        data: Intent,
        host: String = "192.168.0.2",
        port: Int = 50000
    ) {
        val intent = Intent(activity, ScreenCaptureService::class.java).apply {
            putExtra(ScreenCaptureService.EXTRA_RESULT_CODE, resultCode)
            putExtra(ScreenCaptureService.EXTRA_RESULT_DATA, data)
            putExtra(ScreenCaptureService.EXTRA_HOST, host)
            putExtra(ScreenCaptureService.EXTRA_PORT, port)
        }
        activity.startForegroundService(intent)
        Log.i(TAG, "Capture service started: $host:$port")
    }

    /**
     * Stop the capture service.
     */
    fun stopCapture(context: Context) {
        val intent = Intent(context, ScreenCaptureService::class.java)
        context.stopService(intent)
        Log.i(TAG, "Capture service stopped")
    }
}
