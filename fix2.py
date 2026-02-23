import re

p = r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\ipc\AccessoryCommandReceiver.kt'
with open(p,'rb') as f:
    content = f.read().decode('utf-8')

# --- Step 1: companion から videoSocket を削除 ---
old_companion_field = '''        @Volatile
        private var videoSocket: Socket? = null

        fun createIntentFilter'''

new_companion_field = '''        fun createIntentFilter'''

# --- Step 2: instance変数を class本体に追加 (companionの直後、fileDescriptorの前) ---
# class宣言の直後に挿入する
old_class_body_start = '''class AccessoryCommandReceiver : BroadcastReceiver() {
    companion object {'''

new_class_body_start = '''class AccessoryCommandReceiver : BroadcastReceiver() {

    // ✅ FIX-2: companion から instance 変数へ移動
    // ScreenCaptureService と同じライフサイクルで GC される
    @Volatile
    private var videoSocket: Socket? = null

    companion object {'''

# --- Step 3: probeExistingUsb を修正 ---
old_probe = '''    fun probeExistingUsb(svc: ScreenCaptureService) {
        if (videoSocket?.isConnected == true) {
            Log.d(TAG, "probeExistingUsb: already connected, skipping")
            return
        }
        Thread({
            try {
                Thread.sleep(800) // Brief delay for service to fully initialize
                val testSocket = Socket("127.0.0.1", VIDEO_TCP_PORT)
                videoSocket = testSocket
                Log.i(TAG, "probeExistingUsb: found AccessoryIoService on :$VIDEO_TCP_PORT, attaching USB stream")
                svc.attachUsbStream(testSocket.getOutputStream())
            } catch (e: Exception) {
                Log.d(TAG, "probeExistingUsb: no AccessoryIoService on :$VIDEO_TCP_PORT (${e.message})")
            }
        }, "UsbProbe").start()
    }'''

new_probe = '''    fun probeExistingUsb(svc: ScreenCaptureService) {
        // ✅ FIX-2: stale socket を sendUrgentData で実通信確認してからスキップ判定
        val existing = videoSocket
        if (existing != null && !existing.isClosed && existing.isConnected) {
            try {
                existing.sendUrgentData(0) // TCP OOB で生死確認
                Log.d(TAG, "probeExistingUsb: socket alive, skipping")
                return
            } catch (_: Exception) {
                Log.w(TAG, "probeExistingUsb: stale socket detected, reconnecting")
                try { existing.close() } catch (_: Exception) {}
                videoSocket = null
            }
        }
        // ✅ FIX-5: 固定 800ms sleep → 250ms × 8回ポーリング (最大 2秒)
        Thread({
            for (attempt in 1..8) {
                try {
                    val s = Socket("127.0.0.1", VIDEO_TCP_PORT)
                    videoSocket = s
                    Log.i(TAG, "probeExistingUsb: connected (attempt=$attempt)")
                    svc.attachUsbStream(s.getOutputStream())
                    return@Thread
                } catch (_: Exception) {
                    if (attempt < 8) Thread.sleep(250)
                }
            }
            Log.d(TAG, "probeExistingUsb: no AccessoryIoService on :$VIDEO_TCP_PORT")
        }, "UsbProbe").start()
    }'''

errors = []

def apply(old, new, label):
    global content
    old_crlf = old.replace('\n', '\r\n')
    if old_crlf in content:
        content = content.replace(old_crlf, new.replace('\n', '\r\n'))
        print(f"{label}: OK (CRLF)")
    elif old in content:
        content = content.replace(old, new)
        print(f"{label}: OK (LF)")
    else:
        errors.append(label)
        print(f"{label}: ERROR - not found")

apply(old_class_body_start, new_class_body_start, "Step1: class body + instance var")
apply(old_companion_field, new_companion_field, "Step2: remove companion videoSocket")
apply(old_probe, new_probe, "Step3: probeExistingUsb")

if not errors:
    with open(p,'wb') as f:
        f.write(content.encode('utf-8'))
    print("FIX-2 all steps applied, file saved")
else:
    print(f"FAILED steps: {errors}")
