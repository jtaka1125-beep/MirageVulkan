package com.mirage.capture.ipc

import android.util.Log
import com.mirage.capture.capture.ScreenCaptureService
import com.mirage.capture.util.RouteTrace
import com.mirage.capture.usb.Protocol
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.atomic.AtomicBoolean

/**
 * WiFi TCP command server (port 51000).
 * Accepts MIRA binary protocol commands from PC (MirageVulkan) over WiFi.
 * Mirrors AccessoryIoService command handling logic.
 */
object WifiCommandServer {

    private const val TAG = "WifiCmdSrv"
    const val DEFAULT_CMD_PORT = 50001  // tcp_port + 1 (X1: 50000+1, A9#956: 50100+1, A9#479: 50200+1)
    @Volatile var cmdPort: Int = DEFAULT_CMD_PORT

    private val running = AtomicBoolean(false)
    private var serverSocket: ServerSocket? = null
    private var acceptThread: Thread? = null

    fun start(port: Int = DEFAULT_CMD_PORT) {
        cmdPort = port
        if (running.getAndSet(true)) {
            Log.d(TAG, "Already running")
            return
        }
        acceptThread = Thread({
            Log.i(TAG, "Listening on port $cmdPort")
            try {
                val ss = ServerSocket(cmdPort).also { serverSocket = it }
                while (running.get()) {
                    try {
                        val client = ss.accept()
                        Log.i(TAG, "Client connected: ${client.inetAddress.hostAddress}")
                        WifiCommandService.instance?.let { RouteTrace.append(it, "Client connected: ${client.inetAddress.hostAddress}") }
                        Thread({ handleClient(client) }, "WifiCmdClient").start()
                    } catch (e: Exception) {
                        if (running.get()) Log.w(TAG, "accept error: $e")
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "ServerSocket error: $e")
            }
            Log.i(TAG, "Stopped")
        }, "WifiCmdAccept").also { it.isDaemon = true; it.start() }
    }

    fun stop() {
        running.set(false)
        try { serverSocket?.close() } catch (_: Exception) {}
        acceptThread?.interrupt()
        serverSocket = null
        acceptThread = null
        Log.i(TAG, "stop() called")
    }

    private fun handleClient(socket: Socket) {
        try {
            socket.use { s ->
                val ins = s.getInputStream()
                val buf = ByteArray(65536)
                val accum = ArrayList<Byte>(65536)

                while (running.get() && !s.isClosed) {
                    val n = ins.read(buf)
                    WifiCommandService.instance?.let { RouteTrace.append(it, "ins.read n=$n") }
                    if (n <= 0) break
                    for (i in 0 until n) accum.add(buf[i])

                    // Process all complete packets
                    var consumed = true
                    while (consumed) {
                        consumed = false
                        if (accum.size < Protocol.HEADER_SIZE) break

                        val headerBytes = ByteArray(Protocol.HEADER_SIZE) { accum[it] }
                        val hdr = Protocol.parseHeader(headerBytes)
                        if (hdr == null || !hdr.isValid()) {
                            accum.removeAt(0)
                            consumed = true
                            continue
                        }
                        val totalLen = Protocol.HEADER_SIZE + hdr.payloadLen
                        if (accum.size < totalLen) break

                        val pktBytes = ByteArray(totalLen) { accum[it] }
                        repeat(totalLen) { accum.removeAt(0) }
                        consumed = true

                        val rxHdr = "RX header: cmd=0x${hdr.cmd.toInt().and(0xFF).toString(16)} seq=${hdr.seq} len=${hdr.payloadLen}"
                        Log.i(TAG, rxHdr)
                        WifiCommandService.instance?.let { RouteTrace.append(it, rxHdr) }
                        val cmd = Protocol.parseCommand(pktBytes)
                        if (cmd != null) {
                            val parsed = "RX parsed: ${cmd.javaClass.simpleName}"
                            Log.i(TAG, parsed)
                            WifiCommandService.instance?.let { RouteTrace.append(it, parsed) }
                            dispatchCommand(cmd, s)
                        } else {
                            Log.w(TAG, "RX parseCommand returned null")
                            WifiCommandService.instance?.let { RouteTrace.append(it, "RX parseCommand returned null") }
                        }
                    }
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "Client disconnected: $e")
        }
    }

    private fun dispatchCommand(cmd: Protocol.Command, socket: Socket) {
        val accsvc = com.mirage.capture.access.MirageAccessibilityService.instance
        val capsvc = ScreenCaptureService.instance

        when (cmd) {
            is Protocol.Command.Ping -> {
                Log.d(TAG, "PING seq=${cmd.seq}")
                sendAck(socket, cmd.seq, Protocol.STATUS_OK)
            }

            is Protocol.Command.Tap -> {
                Log.d(TAG, "TAP (${cmd.x},${cmd.y}) src=${cmd.w}x${cmd.h}")
                val (tx, ty) = accsvc?.mapFromSource(cmd.x.toFloat(), cmd.y.toFloat(), cmd.w, cmd.h) ?: Pair(cmd.x.toFloat(), cmd.y.toFloat())
                accsvc?.tap(tx, ty, cmd.seq)
                sendAck(socket, cmd.seq, Protocol.STATUS_OK)
            }

            is Protocol.Command.Back -> {
                Log.d(TAG, "BACK seq=${cmd.seq}")
                accsvc?.performBack(cmd.seq)
                sendAck(socket, cmd.seq, Protocol.STATUS_OK)
            }

            is Protocol.Command.Key -> {
                Log.d(TAG, "KEY keycode=${cmd.keycode}")
                accsvc?.performKey(cmd.keycode, cmd.seq)
                sendAck(socket, cmd.seq, Protocol.STATUS_OK)
            }

            is Protocol.Command.Swipe -> {
                Log.d(TAG, "SWIPE (${cmd.startX},${cmd.startY})->(${cmd.endX},${cmd.endY}) dur=${cmd.durationMs}ms src=${cmd.screenW}x${cmd.screenH}")
                val (sx, sy) = accsvc?.mapFromSource(cmd.startX.toFloat(), cmd.startY.toFloat(), cmd.screenW, cmd.screenH) ?: Pair(cmd.startX.toFloat(), cmd.startY.toFloat())
                val (ex, ey) = accsvc?.mapFromSource(cmd.endX.toFloat(), cmd.endY.toFloat(), cmd.screenW, cmd.screenH) ?: Pair(cmd.endX.toFloat(), cmd.endY.toFloat())
                accsvc?.swipe(sx, sy, ex, ey, cmd.durationMs, cmd.seq)
                sendAck(socket, cmd.seq, Protocol.STATUS_OK)
            }

            is Protocol.Command.Pinch -> {
                Log.d(TAG, "PINCH center=(${cmd.centerX},${cmd.centerY})")
                accsvc?.pinch(cmd.centerX.toFloat(), cmd.centerY.toFloat(),
                    cmd.startDistance, cmd.endDistance, cmd.durationMs, cmd.angleDeg100, cmd.seq)
                sendAck(socket, cmd.seq, Protocol.STATUS_OK)
            }

            is Protocol.Command.LongPress -> {
                Log.d(TAG, "LONGPRESS (${cmd.x},${cmd.y}) dur=${cmd.durationMs}ms")
                accsvc?.longPress(cmd.x.toFloat(), cmd.y.toFloat(), cmd.durationMs, cmd.seq)
                sendAck(socket, cmd.seq, Protocol.STATUS_OK)
            }

            is Protocol.Command.ClickId -> {
                Log.d(TAG, "CLICK_ID id=${cmd.resourceId}")
                accsvc?.clickById(cmd.resourceId, cmd.seq)
                sendAck(socket, cmd.seq, Protocol.STATUS_OK)
            }

            is Protocol.Command.ClickText -> {
                Log.d(TAG, "CLICK_TEXT text=${cmd.text}")
                accsvc?.clickByText(cmd.text, cmd.seq)
                sendAck(socket, cmd.seq, Protocol.STATUS_OK)
            }

            is Protocol.Command.VideoFps -> {
                Log.i(TAG, "VIDEO_FPS fps=${cmd.targetFps}")
                capsvc?.updateFps(cmd.targetFps)
                sendAck(socket, cmd.seq, Protocol.STATUS_OK)
            }

            is Protocol.Command.VideoRoute -> {
                Log.i(TAG, "VIDEO_ROUTE mode=${cmd.mode} host=${cmd.host} port=${cmd.port}")
                capsvc?.let { RouteTrace.append(it, "VIDEO_ROUTE mode=${cmd.mode} host=${cmd.host} port=${cmd.port}") }
                when (cmd.mode) {
                    Protocol.VIDEO_ROUTE_USB -> {
                        Log.i(TAG, "VIDEO_ROUTE dispatch USB -> ensureUsbVideoRoute")
                        capsvc?.let { RouteTrace.append(it, "VIDEO_ROUTE dispatch USB -> ensureUsbVideoRoute") }
                        capsvc?.ensureUsbVideoRoute()
                        Log.i(TAG, "VIDEO_ROUTE dispatch USB done")
                        capsvc?.let { RouteTrace.append(it, "VIDEO_ROUTE dispatch USB done") }
                    }
                    2 -> {
                        Log.i(TAG, "VIDEO_ROUTE dispatch TCP")
                        capsvc?.switchSender(ScreenCaptureService.MIRROR_MODE_TCP, null, cmd.port)
                    }
                    else -> {
                        Log.i(TAG, "VIDEO_ROUTE dispatch UDP")
                        capsvc?.switchSender(ScreenCaptureService.MIRROR_MODE_UDP, cmd.host, cmd.port)
                    }
                }
                sendAck(socket, cmd.seq, Protocol.STATUS_OK)
            }

            is Protocol.Command.VideoIdr -> {
                Log.d(TAG, "VIDEO_IDR")
                capsvc?.requestIdr()
                sendAck(socket, cmd.seq, Protocol.STATUS_OK)
            }

            is Protocol.Command.UiTreeReq -> {
                Log.d(TAG, "UI_TREE_REQ")
                val json = accsvc?.dumpUiTree()
                if (json != null) {
                    val pkt = Protocol.buildUiTreeData(cmd.seq, json)
                    try {
                        socket.getOutputStream().write(pkt)
                        socket.getOutputStream().flush()
                    } catch (e: Exception) {
                        Log.w(TAG, "UI_TREE_DATA send error: $e")
                    }
                } else {
                    sendAck(socket, cmd.seq, Protocol.STATUS_ERR_NOT_FOUND)
                }
            }

            is Protocol.Command.Unknown -> {
                Log.w(TAG, "Unknown cmd=0x${cmd.cmd.toInt().and(0xFF).toString(16)}")
                sendAck(socket, cmd.seq, Protocol.STATUS_ERR_UNKNOWN_CMD)
            }

            else -> {
                Log.w(TAG, "Unhandled command: $cmd")
                sendAck(socket, cmd.seq, Protocol.STATUS_ERR_UNKNOWN_CMD)
            }
        }
    }

    private fun sendAck(socket: Socket, seq: Int, status: Byte) {
        try {
            socket.getOutputStream().write(Protocol.buildAck(seq, status))
            socket.getOutputStream().flush()
        } catch (e: Exception) {
            Log.w(TAG, "ACK send error: $e")
        }
    }
}
