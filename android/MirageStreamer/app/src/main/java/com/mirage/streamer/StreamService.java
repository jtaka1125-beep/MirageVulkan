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
import android.os.IBinder;
import android.util.Log;
import android.view.Surface;

import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.nio.ByteBuffer;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Screen capture → H.264 encode → UDP stream service.
 * 
 * Sends raw H.264 NAL units over UDP.
 * Each UDP packet = one NAL unit (with 4-byte start code 00 00 00 01).
 * SPS/PPS sent first, then I-frames and P-frames.
 */
public class StreamService extends Service {
    private static final String TAG = "MirageStream";
    private static final String CHANNEL_ID = "mirage_stream";

    // Retained for Activity restart
    static int lastResultCode;
    static Intent lastResultData;

    private MediaProjection projection;
    private VirtualDisplay virtualDisplay;
    private MediaCodec encoder;
    private DatagramSocket socket;
    private InetAddress targetAddr;
    private int targetPort;
    private Thread encoderThread;
    private final AtomicBoolean running = new AtomicBoolean(false);

    // Stats
    private long frameCount = 0;
    private long bytesSent = 0;
    private long startTime = 0;

    @Override
    public void onCreate() {
        super.onCreate();
        createNotificationChannel();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        // Show foreground notification immediately
        Notification notification = new Notification.Builder(this, CHANNEL_ID)
                .setContentTitle("MirageStreamer")
                .setContentText("Streaming screen...")
                .setSmallIcon(android.R.drawable.ic_menu_camera)
                .build();
        startForeground(1, notification);

        if (intent == null) {
            stopSelf();
            return START_NOT_STICKY;
        }

        // Parse parameters
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

        // Save for re-use
        lastResultCode = resultCode;
        lastResultData = resultData;

        // Stop previous session if any
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
        // UDP socket
        socket = new DatagramSocket();
        targetAddr = InetAddress.getByName(host);
        targetPort = port;
        Log.i(TAG, "UDP target: " + targetAddr + ":" + targetPort);

        // MediaCodec H.264 encoder
        MediaFormat format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height);
        format.setInteger(MediaFormat.KEY_BIT_RATE, bitrate);
        format.setInteger(MediaFormat.KEY_FRAME_RATE, fps);
        format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2);
        format.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);

        encoder = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
        encoder.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        Surface inputSurface = encoder.createInputSurface();
        encoder.start();
        Log.i(TAG, "Encoder started: " + width + "x" + height);

        // MediaProjection → VirtualDisplay → encoder surface
        MediaProjectionManager mpm = (MediaProjectionManager)
                getSystemService(MEDIA_PROJECTION_SERVICE);
        projection = mpm.getMediaProjection(resultCode, resultData);
        
        projection.registerCallback(new MediaProjection.Callback() {
            @Override
            public void onStop() {
                Log.w(TAG, "MediaProjection stopped");
                stopStreaming();
            }
        }, null);

        virtualDisplay = projection.createVirtualDisplay(
                "MirageStreamer",
                width, height, 1,  // density=1
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
                inputSurface, null, null);
        Log.i(TAG, "VirtualDisplay created");

        // Encoder output thread → UDP
        running.set(true);
        startTime = System.currentTimeMillis();
        encoderThread = new Thread(this::encoderLoop, "MirageEncoder");
        encoderThread.start();
    }

    private void encoderLoop() {
        Log.i(TAG, "Encoder loop started");
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        byte[] startCode = {0, 0, 0, 1};

        while (running.get()) {
            try {
                int index = encoder.dequeueOutputBuffer(info, 10_000); // 10ms timeout
                if (index >= 0) {
                    ByteBuffer buf = encoder.getOutputBuffer(index);
                    if (buf != null && info.size > 0) {
                        // Extract NAL data
                        byte[] nalData = new byte[info.size];
                        buf.position(info.offset);
                        buf.get(nalData, 0, info.size);

                        // Send over UDP with start code prefix
                        sendNalUnit(nalData);

                        frameCount++;
                        bytesSent += nalData.length;

                        // Log stats every 60 frames
                        if (frameCount % 60 == 0) {
                            long elapsed = System.currentTimeMillis() - startTime;
                            double avgFps = frameCount * 1000.0 / elapsed;
                            double mbps = bytesSent * 8.0 / elapsed / 1000.0;
                            Log.i(TAG, String.format("UDP: frames=%d fps=%.1f bitrate=%.1f Mbps",
                                    frameCount, avgFps, mbps));
                        }
                    }
                    encoder.releaseOutputBuffer(index, false);
                } else if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    Log.i(TAG, "Encoder format changed: " + encoder.getOutputFormat());
                }
            } catch (Exception e) {
                if (running.get()) {
                    Log.e(TAG, "Encoder loop error", e);
                }
                break;
            }
        }
        Log.i(TAG, "Encoder loop ended (frames=" + frameCount + ")");
    }

    /**
     * Send H.264 NAL unit over UDP.
     * If NAL > 1400 bytes, fragment into FU-A packets for MTU safety.
     */
    private void sendNalUnit(byte[] nal) {
        try {
            // Find actual NAL start (skip any start codes already in data)
            int offset = 0;
            if (nal.length > 4 && nal[0] == 0 && nal[1] == 0 && nal[2] == 0 && nal[3] == 1) {
                offset = 4;
            } else if (nal.length > 3 && nal[0] == 0 && nal[1] == 0 && nal[2] == 1) {
                offset = 3;
            }

            int nalLen = nal.length - offset;
            if (nalLen <= 0) return;

            if (nalLen <= 1400) {
                // Send as single packet: start_code + NAL
                byte[] pkt = new byte[4 + nalLen];
                System.arraycopy(new byte[]{0, 0, 0, 1}, 0, pkt, 0, 4);
                System.arraycopy(nal, offset, pkt, 4, nalLen);
                socket.send(new DatagramPacket(pkt, pkt.length, targetAddr, targetPort));
            } else {
                // Fragment: send multiple packets with start code prefix
                // Simple fragmentation: just split into chunks
                int chunkSize = 1400;
                for (int i = 0; i < nalLen; i += chunkSize) {
                    int len = Math.min(chunkSize, nalLen - i);
                    byte[] pkt;
                    if (i == 0) {
                        // First fragment includes start code
                        pkt = new byte[4 + len];
                        System.arraycopy(new byte[]{0, 0, 0, 1}, 0, pkt, 0, 4);
                        System.arraycopy(nal, offset + i, pkt, 4, len);
                    } else {
                        pkt = new byte[len];
                        System.arraycopy(nal, offset + i, pkt, 0, len);
                    }
                    socket.send(new DatagramPacket(pkt, pkt.length, targetAddr, targetPort));
                }
            }
        } catch (Exception e) {
            if (running.get()) {
                Log.w(TAG, "UDP send error: " + e.getMessage());
            }
        }
    }

    private void stopStreaming() {
        running.set(false);

        if (encoderThread != null) {
            try { encoderThread.join(2000); } catch (InterruptedException ignored) {}
            encoderThread = null;
        }
        if (virtualDisplay != null) {
            virtualDisplay.release();
            virtualDisplay = null;
        }
        if (projection != null) {
            projection.stop();
            projection = null;
        }
        if (encoder != null) {
            try { encoder.stop(); } catch (Exception ignored) {}
            try { encoder.release(); } catch (Exception ignored) {}
            encoder = null;
        }
        if (socket != null) {
            socket.close();
            socket = null;
        }
        Log.i(TAG, "Streaming stopped (total frames=" + frameCount + ")");
    }

    private void createNotificationChannel() {
        NotificationChannel ch = new NotificationChannel(CHANNEL_ID, "MirageStreamer",
                NotificationManager.IMPORTANCE_LOW);
        ch.setDescription("Screen streaming notification");
        getSystemService(NotificationManager.class).createNotificationChannel(ch);
    }

    @Override
    public void onDestroy() {
        stopStreaming();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
