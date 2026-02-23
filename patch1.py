
OLD = 'fun detachUsbStream() {\r\n        if (videoSender !is UsbVideoSender) return\r\n        Log.i(TAG, "USB disconnected, stopping video")\r\n        stopTcpSecondary()\r\n        encoder?.stop()\r\n        videoSender?.close()\r\n        videoSender = null\r\n        encoder = null\r\n        mirrorMode = MIRROR_MODE_UDP\r\n    }'

NEW = 'fun detachUsbStream() {\r\n        if (videoSender !is UsbVideoSender) return\r\n        val proj = projection\r\n        if (proj == null) {\r\n            Log.w(TAG, "USB detached but projection is null, cannot restore UDP")\r\n            stopTcpSecondary()\r\n            encoder?.stop()\r\n            videoSender?.close()\r\n            videoSender = null\r\n            encoder = null\r\n            mirrorMode = MIRROR_MODE_UDP\r\n            return\r\n        }\r\n        Log.i(TAG, "USB disconnected, restoring UDP \\u2192 $lastHost:$lastPort")\r\n        stopTcpSecondary()\r\n        encoder?.stop()\r\n        videoSender?.close()\r\n        // Restart encoder with UDP sender (projection still valid, no re-consent needed)\r\n        val udpSender = UdpVideoSender(lastHost, lastPort)\r\n        videoSender = udpSender\r\n        mirrorMode = MIRROR_MODE_UDP\r\n        encoder = H264Encoder(this, proj, udpSender)\r\n        encoder?.start()\r\n        startTcpSecondary()\r\n        Log.i(TAG, "UDP restored: $lastHost:$lastPort")\r\n    }'

path = r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\capture\ScreenCaptureService.kt'
with open(path, 'rb') as f:
    text = f.read().decode('utf-8')

if OLD not in text:
    print('ERROR: block not found')
else:
    new_text = text.replace(OLD, NEW, 1)
    with open(path, 'wb') as f:
        f.write(new_text.encode('utf-8'))
    print('OK: detachUsbStream patched')
    with open(path, 'rb') as f:
        check = f.read().decode('utf-8')
    print('VERIFY:', 'UDP restored' in check)
