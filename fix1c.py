
p = r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\usb\AccessoryIoService.kt'
with open(p,'rb') as f: raw = f.read()
content = raw.decode('utf-8')

old_start = content.find('    private fun startVideoForward() {')
old_end = content.find('    private fun stopVideoForward() {')
old = content[old_start:old_end].rstrip()

new = '''    private fun startVideoForward() {
        stopVideoForward()
        videoForwardThread = Thread({
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_AUDIO)
            Log.i(TAG, "Video forward thread started")

            // \u2705 FIX-1: ServerSocket \u306f\u4e00\u5ea6\u3060\u3051\u4f5c\u6210\u3057\u3001\u518d\u63a5\u7d9a\u30eb\u30fc\u30d7\u3067\u4f7f\u3044\u56de\u3059
            try {
                videoServerSocket = ServerSocket().also {
                    it.reuseAddress = true
                    it.bind(java.net.InetSocketAddress(InetAddress.getByName("127.0.0.1"), VIDEO_TCP_PORT), 1)
                }
                Log.i(TAG, "TCP ServerSocket listening on localhost:$VIDEO_TCP_PORT")
            } catch (e: IOException) {
                Log.e(TAG, "Failed to bind ServerSocket on :$VIDEO_TCP_PORT", e)
                return@Thread
            }

            // \u2705 FIX-1: \u5916\u5074\u30eb\u30fc\u30d7 \u2014 MirageCapture \u518d\u8d77\u52d5\u306e\u305f\u3073\u306b accept() \u3092\u7e70\u308a\u8fd4\u3059
            while (running.get()) {
                try {
                    Log.i(TAG, "Waiting for MirageCapture on :$VIDEO_TCP_PORT...")
                    val client = videoServerSocket?.accept() ?: break
                    videoClientSocket = client
                    client.tcpNoDelay = true
                    client.receiveBufferSize = 256 * 1024
                    Log.i(TAG, "MirageCapture connected")

                    // \u5185\u5074\u30eb\u30fc\u30d7: 1\u63a5\u7d9a\u306e\u30c7\u30fc\u30bf\u8ee2\u9001
                    val clientIn = client.inputStream
                    val buf = ByteArray(131072)
                    var totalBytes = 0L
                    var lastLogTime = System.currentTimeMillis()

                    while (running.get()) {
                        val n = clientIn.read(buf)
                        if (n < 0) {
                            Log.i(TAG, "MirageCapture disconnected (EOF), waiting for reconnect...")
                            break  // \u5916\u5074\u30eb\u30fc\u30d7\u306b\u623a\u308a\u6b21\u306e accept() \u3078
                        }
                        if (n == 0) continue
                        outputStream?.write(buf, 0, n)
                        totalBytes += n
                        val now = System.currentTimeMillis()
                        if (now - lastLogTime >= 5000) {
                            Log.i(TAG, "Video fwd: ${totalBytes / 1024}KB total, last ${n}B")
                            lastLogTime = now
                        }
                    }

                    // \u5207\u65ad\u30af\u30ea\u30fc\u30f3\u30a2\u30c3\u30d7 (ServerSocket \u306f\u7dad\u6301\u3057\u3066\u6b21\u306e accept() \u3078)
                    try { videoClientSocket?.close() } catch (_: Exception) {}
                    videoClientSocket = null

                } catch (e: IOException) {
                    if (!running.get()) break
                    Log.w(TAG, "Video forward accept error: ${e.message}, retry in 1s")
                    try { Thread.sleep(1000) } catch (_: InterruptedException) { break }
                }
            }

            Log.i(TAG, "Video forward thread ended")
        }, "VideoForward").also { it.start() }
    }'''

# Normalize new to CRLF
new_crlf = new.replace('\r\n', '\n').replace('\n', '\r\n')

result = content[:old_start] + new_crlf + '\r\n\r\n' + content[old_end:]
with open(p,'wb') as f: f.write(result.encode('utf-8'))
print("OK: FIX-1 applied")
