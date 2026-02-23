
p = r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\usb\AccessoryIoService.kt'
with open(p,'rb') as f: content = f.read().decode('utf-8')

old = '''    private fun startVideoForward() {
        stopVideoForward()
        videoForwardThread = Thread({
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_AUDIO)
            Log.i(TAG, "Video forward thread started")
            try {
                videoServerSocket = ServerSocket().also { it.reuseAddress = true; it.bind(java.net.InetSocketAddress(InetAddress.getByName("127.0.0.1"), VIDEO_TCP_PORT), 1) }
                Log.i(TAG, "TCP ServerSocket listening on localhost:$VIDEO_TCP_PORT")

                // Accept one client (MirageCapture)
                videoClientSocket = videoServerSocket?.accept()
                Log.i(TAG, "Video client connected")
                videoClientSocket?.tcpNoDelay = true
                videoClientSocket?.receiveBufferSize = 256 * 1024

                val clientIn = videoClientSocket?.inputStream ?: return@Thread
                val buf = ByteArray(131072)
                var totalBytes = 0L
                var lastLogTime = System.currentTimeMillis()

                while (running.get()) {
                    val n = clientIn.read(buf)
                    if (n < 0) break
                    if (n == 0) continue

                    outputStream?.write(buf, 0, n)
                    totalBytes += n

                    val now = System.currentTimeMillis()
                    if (now - lastLogTime >= 5000) {
                        Log.i(TAG, "Video fwd: ${totalBytes / 1024}KB total, last ${n}B")
                        lastLogTime = now
                    }
                }
            } catch (e: IOException) {
                if (running.get()) Log.e(TAG, "Video forward error", e)
            } finally {
                Log.i(TAG, "Video forward thread ended")
            }
        }, "VideoForward").also { it.start() }
    }'''

new = '''    private fun startVideoForward() {
        stopVideoForward()
        videoForwardThread = Thread({
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_AUDIO)
            Log.i(TAG, "Video forward thread started")

            // ✅ FIX-1: ServerSocket は一度だけ作成し、再接続ループで使い回す
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

            // ✅ FIX-1: 外側ループ — MirageCapture 再起動のたびに accept() を繰り返す
            while (running.get()) {
                try {
                    Log.i(TAG, "Waiting for MirageCapture on :$VIDEO_TCP_PORT...")
                    val client = videoServerSocket?.accept() ?: break
                    videoClientSocket = client
                    client.tcpNoDelay = true
                    client.receiveBufferSize = 256 * 1024
                    Log.i(TAG, "MirageCapture connected from ${client.remoteSocketAddress}")

                    // 内側ループ: 1接続のデータ転送
                    val clientIn = client.inputStream
                    val buf = ByteArray(131072)
                    var totalBytes = 0L
                    var lastLogTime = System.currentTimeMillis()

                    while (running.get()) {
                        val n = clientIn.read(buf)
                        if (n < 0) {
                            Log.i(TAG, "MirageCapture disconnected (EOF), waiting for reconnect...")
                            break  // 外側ループに戻り次の accept() を待つ
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

                    // 切断クリーンアップ (ServerSocket は維持して次の accept() へ)
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

if old in content:
    content = content.replace(old, new)
    with open(p,'wb') as f: f.write(content.encode('utf-8'))
    print("OK: startVideoForward replaced")
else:
    print("ERROR: pattern not found")
    # Debug
    idx = content.find('private fun startVideoForward')
    print(repr(content[idx:idx+200]))
