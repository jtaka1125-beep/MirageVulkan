package com.mirage.streamer;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;
import android.widget.Toast;

/**
 * MirageStreamer - Screen capture to UDP H.264 stream
 * 
 * Launch via ADB:
 *   am start -n com.mirage.streamer/.StreamActivity \
 *     --es host 192.168.0.7 --ei port 60000
 * 
 * Or with auto-start (skips manual tap):
 *   am start -n com.mirage.streamer/.StreamActivity \
 *     --es host 192.168.0.7 --ei port 60000 --ez auto true
 */
public class StreamActivity extends Activity {
    private static final String TAG = "MirageStream";
    private static final int REQUEST_MEDIA_PROJECTION = 1001;

    private String host = "192.168.0.7";
    private int port = 60000;
    private int width = 720;
    private int height = 1280;
    private int fps = 30;
    private int bitrate = 2_000_000;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Parse intent extras
        Intent intent = getIntent();
        if (intent != null) {
            host = intent.getStringExtra("host") != null ? intent.getStringExtra("host") : host;
            port = intent.getIntExtra("port", port);
            width = intent.getIntExtra("width", width);
            height = intent.getIntExtra("height", height);
            fps = intent.getIntExtra("fps", fps);
            bitrate = intent.getIntExtra("bitrate", bitrate);
        }

        Log.i(TAG, "Target: " + host + ":" + port + " " + width + "x" + height + "@" + fps);

        // Simple status UI
        TextView tv = new TextView(this);
        tv.setTextSize(16f);
        tv.setPadding(32, 32, 32, 32);
        tv.setText("MirageStreamer\n\nTarget: " + host + ":" + port +
                   "\nResolution: " + width + "x" + height + "@" + fps +
                   "\n\nRequesting screen capture permission...");
        setContentView(tv);

        // Request MediaProjection
        MediaProjectionManager mpm = (MediaProjectionManager)
            getSystemService(Context.MEDIA_PROJECTION_SERVICE);
        startActivityForResult(mpm.createScreenCaptureIntent(), REQUEST_MEDIA_PROJECTION);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        // Re-parse and restart with new host/port
        if (intent != null) {
            String newHost = intent.getStringExtra("host");
            int newPort = intent.getIntExtra("port", -1);
            if (newHost != null && newPort > 0) {
                host = newHost;
                port = newPort;
                Log.i(TAG, "New target: " + host + ":" + port);
                // Stop existing service and restart
                stopService(new Intent(this, StreamService.class));
                startStreamService(StreamService.lastResultCode, StreamService.lastResultData);
            }
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == REQUEST_MEDIA_PROJECTION) {
            if (resultCode == Activity.RESULT_OK && data != null) {
                Log.i(TAG, "MediaProjection permission granted");
                startStreamService(resultCode, data);
                // Minimize to background
                moveTaskToBack(true);
            } else {
                Log.e(TAG, "MediaProjection permission denied");
                Toast.makeText(this, "Screen capture permission denied", Toast.LENGTH_LONG).show();
                finish();
            }
        }
    }

    private void startStreamService(int resultCode, Intent resultData) {
        Intent svc = new Intent(this, StreamService.class);
        svc.putExtra("resultCode", resultCode);
        svc.putExtra("resultData", resultData);
        svc.putExtra("host", host);
        svc.putExtra("port", port);
        svc.putExtra("width", width);
        svc.putExtra("height", height);
        svc.putExtra("fps", fps);
        svc.putExtra("bitrate", bitrate);
        startForegroundService(svc);
        Log.i(TAG, "StreamService started -> " + host + ":" + port);
    }
}
