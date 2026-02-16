package com.mirage.streamer;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Surface;
import android.view.WindowManager;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.nio.ByteBuffer;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Screen capture -> H.264 encode -> UDP stream service.
 * Uses async MediaCodec callback for MediaTek compatibility.
 * KEY_REPEAT_PREVIOUS_FRAME_AFTER forces encoder output even on static screens.
 */
public class StreamService extends Service {
    private static final String TAG = "MirageStream";
    private static final String CHANNEL_ID = "mirage_stream";

    static int lastResultCode;
    static Intent lastResultData;

    private MediaProjection projection;
    private VirtualDisplay virtualDisplay;
    private MediaCodec encoder;
    private DatagramSocket socket;
    private InetAddress targetAddr;
    private int targetPort;
    private HandlerThread codecThread;

    private final AtomicLong frameCount = new AtomicLong(0);
    private final AtomicLong bytesSent = new AtomicLong(0);
    private long startTime = 0;

    @Override
    public void onCreate() {
        super.onCreate();
        createNotificationChannel();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Notification notification = new Notification.Builder(this, CHANNEL_ID)
                .setContentTitle("MirageStreamer")
                .setContentText("Streaming screen...")
                .setSmallIcon(android.R.drawable.ic_menu_camera)
                .build();
        startForeground(1, notification);

        if (intent == null) { stopSelf(); return START_NOT_STICKY; }

        int resultCode = intent.getIntExtra("resultCode", 0);
        Intent resultData = intent.getParcelableExtra("resultData");
        String host = intent.getStringExtra("host");
        int port = intent.getIntExtra("port", 60000);
        int width = intent.getIntExtra("width", 720);
        int height = intent.getIntExtra("height", 1280);
        int fps = intent.getIntExtra("fps", 30);
        int bitrate = intent.getIntExtra("bitrate", 2_000_000);

        if (resultData == null || host == null) {
            Log.e(TAG, "Missing resultData or host");
            stopSelf();
            return START_NOT_STICKY;
        }

        lastResultCode = resultCode;
        lastResultData = resultData;
        stopStreaming();

        Log.i(TAG, String.format("Starting stream: %s:%d %dx%d@%d %dkbps",
                host, port, width, height, fps, bitrate / 1000));

        try {
            startStreaming(resultCode, resultData, host, port, width, height, fps, bitrate);
        } catch (Exception e) {
            Log.e(TAG, "Failed to start streaming", e);
            stopSelf();
        }
        return START_REDELIVER_INTENT;
    }

    private void startStreaming(int resultCode, Intent resultData,
                                String host, int port,
                                int width, int height, int fps, int bitrate) throws Exception {
        socket = new DatagramSocket();
        targetAddr = InetAddress.getByName(host);
        targetPort = port;
        Log.i(TAG, "UDP target: " + targetAddr + ":" + targetPort);

        // Codec handler thread
        codecThread = new HandlerThread("MirageCodec");
        codecThread.start();
        Handler codecHandler = new Handler(codecThread.getLooper());

        // MediaCodec H.264 encoder
        MediaFormat format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height);
        format.setInteger(MediaFormat.KEY_BIT_RATE, bitrate);
        format.setInteger(MediaFormat.KEY_FRAME_RATE, fps);
        format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2);
        format.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
        // Force encoder to repeat frames even when screen is static
        format.setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, 100_000); // 100ms in microseconds

        encoder = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
        startTime = System.currentTimeMillis();

        encoder.setCallback(new MediaCodec.Callback() {
            @Override
            public void onInputBufferAvailable(MediaCodec codec, int index) {}

            @Override
            public void onOutputBufferAvailable(MediaCodec codec, int index, MediaCodec.BufferInfo info) {
                try {
                    ByteBuffer buf = codec.getOutputBuffer(index);
                    if (buf != null && info.size > 0) {
                        byte[] nalData = new byte[info.size];
                        buf.position(info.offset);
                        buf.get(nalData, 0, info.size);
                        sendNalUnit(nalData);

                        long fc = frameCount.incrementAndGet();
                        bytesSent.addAndGet(nalData.length);

                        if (fc == 1) {
                            Log.i(TAG, "First frame sent! size=" + nalData.length);
                        }
                        if (fc % 60 == 0) {
                            long elapsed = System.currentTimeMillis() - startTime;
                            double avgFps = fc * 1000.0 / elapsed;
                            double mbps = bytesSent.get() * 8.0 / elapsed / 1000.0;
                            Log.i(TAG, String.format("UDP: frames=%d fps=%.1f bitrate=%.1f Mbps",
                                    fc, avgFps, mbps));
                        }
                    }
                    codec.releaseOutputBuffer(index, false);
                } catch (Exception e) {
                    Log.e(TAG, "Output buffer error", e);
                }
            }

            @Override
            public void onError(MediaCodec codec, MediaCodec.CodecException e) {
                Log.e(TAG, "Encoder error: " + e.getDiagnosticInfo(), e);
            }

            @Override
            public void onOutputFormatChanged(MediaCodec codec, MediaFormat fmt) {
                Log.i(TAG, "Encoder format: " + fmt);
            }
        }, codecHandler);

        encoder.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        Surface inputSurface = encoder.createInputSurface();
        encoder.start();
        Log.i(TAG, "Encoder started (async): " + width + "x" + height);

        // Get MediaProjection
        MediaProjectionManager mpm = (MediaProjectionManager)
                getSystemService(MEDIA_PROJECTION_SERVICE);
        projection = mpm.getMediaProjection(resultCode, resultData);

        projection.registerCallback(new MediaProjection.Callback() {
            @Override
            public void onStop() {
                Log.w(TAG, "MediaProjection stopped externally");
                stopStreaming();
            }
        }, null);

        // Get screen density
        DisplayMetrics dm = new DisplayMetrics();
        ((WindowManager) getSystemService(WINDOW_SERVICE)).getDefaultDisplay().getMetrics(dm);
        int dpi = dm.densityDpi;
        Log.i(TAG, "Screen density: " + dpi + " dpi");

        // Create VirtualDisplay - use flag 0 for maximum compatibility
        // AUTO_MIRROR can cause issues on some Android 14/15 devices
        virtualDisplay = projection.createVirtualDisplay(
                "MirageStreamer",
                width, height, dpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR
                    | DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC,
                inputSurface, null, null);
        Log.i(TAG, "VirtualDisplay created (AUTO_MIRROR|PUBLIC) -> streaming!");
    }

    private void sendNalUnit(byte[] nal) {
        try {
            int offset = 0;
            if (nal.length > 4 && nal[0] == 0 && nal[1] == 0 && nal[2] == 0 && nal[3] == 1) {
                offset = 4;
            } else if (nal.length > 3 && nal[0] == 0 && nal[1] == 0 && nal[2] == 1) {
                offset = 3;
            }
            int nalLen = nal.length - offset;
            if (nalLen <= 0) return;

            if (nalLen <= 1400) {
                byte[] pkt = new byte[4 + nalLen];
                pkt[0] = 0; pkt[1] = 0; pkt[2] = 0; pkt[3] = 1;
                System.arraycopy(nal, offset, pkt, 4, nalLen);
                socket.send(new DatagramPacket(pkt, pkt.length, targetAddr, targetPort));
            } else {
                int chunkSize = 1400;
                for (int i = 0; i < nalLen; i += chunkSize) {
                    int len = Math.min(chunkSize, nalLen - i);
                    byte[] pkt;
                    if (i == 0) {
                        pkt = new byte[4 + len];
                        pkt[0] = 0; pkt[1] = 0; pkt[2] = 0; pkt[3] = 1;
                        System.arraycopy(nal, offset + i, pkt, 4, len);
                    } else {
                        pkt = new byte[len];
                        System.arraycopy(nal, offset + i, pkt, 0, len);
                    }
                    socket.send(new DatagramPacket(pkt, pkt.length, targetAddr, targetPort));
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "UDP send error: " + e.getMessage());
        }
    }

    private void stopStreaming() {
        if (virtualDisplay != null) { virtualDisplay.release(); virtualDisplay = null; }
        if (projection != null) { projection.stop(); projection = null; }
        if (encoder != null) {
            try { encoder.stop(); } catch (Exception ignored) {}
            try { encoder.release(); } catch (Exception ignored) {}
            encoder = null;
        }
        if (codecThread != null) { codecThread.quitSafely(); codecThread = null; }
        if (socket != null) { socket.close(); socket = null; }
        Log.i(TAG, "Streaming stopped (total frames=" + frameCount.get() + ")");
    }

    private void createNotificationChannel() {
        NotificationChannel ch = new NotificationChannel(CHANNEL_ID, "MirageStreamer",
                NotificationManager.IMPORTANCE_LOW);
        getSystemService(NotificationManager.class).createNotificationChannel(ch);
    }

    @Override
    public void onDestroy() { stopStreaming(); super.onDestroy(); }

    @Override
    public IBinder onBind(Intent intent) { return null; }
}
