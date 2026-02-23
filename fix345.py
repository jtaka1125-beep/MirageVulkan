import re

# FIX-3: VideoSender.flush() + H264Encoder instanceof 削除
# FIX-4: commandReceiver dead code 削除
# FIX-6: MediaProjection packageName 緩和
# FIX-7: accessory minSdk 29

import os

def apply_patch(path, patches):
    with open(path,'rb') as f:
        content = f.read().decode('utf-8')
    errors = []
    for label, old, new in patches:
        old_crlf = old.replace('\n', '\r\n')
        if old_crlf in content:
            content = content.replace(old_crlf, new.replace('\n', '\r\n'))
            print(f"  {label}: OK")
        elif old in content:
            content = content.replace(old, new)
            print(f"  {label}: OK (LF)")
        else:
            errors.append(label)
            print(f"  {label}: ERROR")
    if not errors:
        with open(path,'wb') as f:
            f.write(content.encode('utf-8'))
    return errors

# ── FIX-3: VideoSender.kt ──
print("=== FIX-3: VideoSender.flush() ===")
apply_patch(
    r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\capture\VideoSender.kt',
    [('add flush()',
    '''    /**
     * Close the sender and release resources.
     */
    fun close()
}''',
    '''    /**
     * Close the sender and release resources.
     */
    fun close()

    /**
     * Flush buffered data at frame boundary.
     * Default is no-op for non-buffered implementations.
     */
    fun flush() {}
}'''
    )]
)

# ── FIX-3: UsbVideoSender.kt - flush() override 追加 ──
print("=== FIX-3: UsbVideoSender flush override ===")
usb_path = r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\capture\UsbVideoSender.kt'
with open(usb_path,'rb') as f:
    usb = f.read().decode('utf-8')
# flushBatch() の後に override flush() を追加
old_flush = '    fun flushBatch() {'
new_flush = '''    override fun flush() = flushBatch()

    fun flushBatch() {'''
usb_crlf = old_flush.replace('\n','\r\n')
if usb_crlf in usb:
    usb = usb.replace(usb_crlf, new_flush.replace('\n','\r\n'))
    print("  UsbVideoSender flush override: OK")
elif old_flush in usb:
    usb = usb.replace(old_flush, new_flush)
    print("  UsbVideoSender flush override: OK (LF)")
else:
    print("  UsbVideoSender flush override: ERROR")
with open(usb_path,'wb') as f:
    f.write(usb.encode('utf-8'))

# ── FIX-3: H264Encoder.kt - instanceof → flush() ──
print("=== FIX-3: H264Encoder instanceof removal ===")
h264_path = r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\capture\H264Encoder.kt'
with open(h264_path,'rb') as f:
    h264 = f.read().decode('utf-8')
print("  Searching for instanceof pattern...")
# 実際のコード確認
idx = h264.find('UsbVideoSender')
if idx >= 0:
    print("  Found UsbVideoSender at:", idx)
    print("  Context:", repr(h264[max(0,idx-100):idx+200]))
else:
    print("  UsbVideoSender not found in H264Encoder")
