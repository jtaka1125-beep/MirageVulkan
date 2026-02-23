package com.mirage.capture.capture

import android.graphics.SurfaceTexture
import android.opengl.*
import android.opengl.GLES11Ext
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer

/**
 * TileRepeater
 *
 * Like SurfaceRepeater, but draws the same input frame into N encoder surfaces (tiles)
 * by cropping via texture coordinates. This enables generic (device-agnostic) tiling
 * to bypass hardware encoder max resolution limits while keeping frames synchronized.
 *
 * Key properties:
 *  - Single input SurfaceTexture fed by one VirtualDisplay (targetW x targetH)
 *  - Multiple output EGL window surfaces (one per encoder input Surface)
 *  - Each tile receives the SAME presentation timestamp (SurfaceTexture.timestamp)
 *    via EGLExt.eglPresentationTimeANDROID to keep encoders in lock-step.
 */
class TileRepeater(
    private val outputs: List<TileOutput>,
    private val targetW: Int,
    private val targetH: Int,
    @Volatile private var targetFps: Int = 30
) {
    data class TileOutput(
        val tileIndex: Int,
        val tilesX: Int,
        val tilesY: Int,
        val tileX: Int,
        val tileY: Int,
        val tileW: Int,
        val tileH: Int,
        val outputSurface: Surface,
    )

    companion object {
        private const val TAG = "TileRepeater"

        private const val VERTEX_SHADER = """
            attribute vec4 aPosition;
            attribute vec2 aTexCoord;
            varying vec2 vTexCoord;
            void main() {
                gl_Position = aPosition;
                vTexCoord = aTexCoord;
            }
        """

        private const val FRAGMENT_SHADER = """
            #extension GL_OES_EGL_image_external : require
            precision mediump float;
            varying vec2 vTexCoord;
            uniform samplerExternalOES uTexture;
            uniform mat4 uTexMatrix;
            void main() {
                vec2 tc = (uTexMatrix * vec4(vTexCoord, 0.0, 1.0)).xy;
                gl_FragColor = texture2D(uTexture, tc);
            }
        """

        private val QUAD_VERTICES = floatArrayOf(
            -1f, -1f,
             1f, -1f,
            -1f,  1f,
             1f,  1f
        )

        private fun makeTexCoords(u0: Float, v0: Float, u1: Float, v1: Float): FloatArray {
            // Triangle strip
            return floatArrayOf(
                u0, v0,
                u1, v0,
                u0, v1,
                u1, v1
            )
        }
    }

    // EGL objects
    private var eglDisplay: EGLDisplay = EGL14.EGL_NO_DISPLAY
    private var eglContext: EGLContext = EGL14.EGL_NO_CONTEXT
    private var eglConfig: EGLConfig? = null

    private data class OutEgl(val out: TileOutput, var eglSurface: EGLSurface)
    private val eglOuts = ArrayList<OutEgl>()

    // GL objects
    private var program = 0
    private var texId = 0
    private var aPositionLoc = 0
    private var aTexCoordLoc = 0
    private var uTextureLoc = 0
    private var uTexMatrixLoc = 0

    // SurfaceTexture input
    private var surfaceTexture: SurfaceTexture? = null
    var inputSurface: Surface? = null
        private set

    @Volatile private var running = false
    @Volatile private var hasNewFrame = false
    private var renderThread: Thread? = null

    private val texMatrix = FloatArray(16)
    private var lastPtsNs: Long = 0L

    private lateinit var vertexBuffer: FloatBuffer
    private lateinit var texCoordBuffer: FloatBuffer

    fun start() {
        if (outputs.isEmpty()) {
            Log.e(TAG, "No outputs")
            return
        }
        running = true
        renderThread = Thread({ renderLoop() }, "TileRepeater").also { it.start() }

        val deadline = System.currentTimeMillis() + 3000
        while (inputSurface == null && System.currentTimeMillis() < deadline) {
            Thread.sleep(10)
        }
        if (inputSurface == null) {
            Log.e(TAG, "Failed to create inputSurface within timeout")
            stop()
        } else {
            Log.i(TAG, "Started: target=${targetW}x${targetH} tiles=${outputs.size} @${targetFps}fps")
        }
    }

    fun stop() {
        running = false
        renderThread?.join(2000)
        renderThread = null
        Log.i(TAG, "Stopped")
    }

    fun updateFps(newFps: Int) {
        val clamped = newFps.coerceIn(10, 60)
        if (clamped != targetFps) {
            Log.i(TAG, "FPS updated: $targetFps -> $clamped")
            targetFps = clamped
        }
    }

    private fun renderLoop() {
        try {
            initEgl()
            initGl()
            createSurfaceTexture()
            createOutputSurfaces()

            var frameCount = 0L
            var lastLogTime = System.currentTimeMillis()

            while (running) {
                val frameStartNs = System.nanoTime()

                if (hasNewFrame) {
                    surfaceTexture?.updateTexImage()
                    surfaceTexture?.getTransformMatrix(texMatrix)
                    lastPtsNs = surfaceTexture?.timestamp ?: frameStartNs
                    hasNewFrame = false
                }

                // Draw same frame into each tile surface
                for (oe in eglOuts) {
                    if (!EGL14.eglMakeCurrent(eglDisplay, oe.eglSurface, oe.eglSurface, eglContext)) {
                        Log.w(TAG, "eglMakeCurrent failed tile=${oe.out.tileIndex}")
                        continue
                    }

                    // set viewport to tile output size
                    GLES20.glViewport(0, 0, oe.out.tileW, oe.out.tileH)

                    // compute normalized crop rect in source space
                    val u0 = oe.out.tileX.toFloat() / oe.out.tilesX.toFloat()
                    val u1 = (oe.out.tileX + 1).toFloat() / oe.out.tilesX.toFloat()
                    val v0 = oe.out.tileY.toFloat() / oe.out.tilesY.toFloat()
                    val v1 = (oe.out.tileY + 1).toFloat() / oe.out.tilesY.toFloat()

                    val tc = makeTexCoords(u0, v0, u1, v1)
                    texCoordBuffer.clear()
                    texCoordBuffer.put(tc)
                    texCoordBuffer.position(0)

                    drawFrame()

                    // Set same timestamp for all tiles (sync)
                    try {
                        EGLExt.eglPresentationTimeANDROID(eglDisplay, oe.eglSurface, lastPtsNs)
                    } catch (_: Throwable) {
                        // ignore if extension missing
                    }

                    EGL14.eglSwapBuffers(eglDisplay, oe.eglSurface)
                }

                frameCount++

                val now = System.currentTimeMillis()
                if (now - lastLogTime >= 5000) {
                    val elapsed = (now - lastLogTime) / 1000.0
                    val fps = frameCount / elapsed
                    Log.i(TAG, "Repeater: %.1f fps (%d frames in %.1fs)".format(fps, frameCount, elapsed))
                    frameCount = 0
                    lastLogTime = now
                }

                val frameIntervalNs = 1_000_000_000L / targetFps
                val targetTimeNs = frameStartNs + frameIntervalNs
                val nowNs = System.nanoTime()
                val remainingNs = targetTimeNs - nowNs
                if (remainingNs > 2_000_000L) {
                    Thread.sleep((remainingNs - 2_000_000L) / 1_000_000L)
                }
                while (System.nanoTime() < targetTimeNs) {
                    Thread.yield()
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Render loop error", e)
        } finally {
            releaseGl()
            releaseEgl()
        }
    }

    private fun initEgl() {
        eglDisplay = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        if (eglDisplay == EGL14.EGL_NO_DISPLAY) throw RuntimeException("eglGetDisplay failed")

        val version = IntArray(2)
        if (!EGL14.eglInitialize(eglDisplay, version, 0, version, 1)) {
            throw RuntimeException("eglInitialize failed")
        }

        val EGL_RECORDABLE_ANDROID = 0x3142
        val configAttribs = intArrayOf(
            EGL14.EGL_RED_SIZE, 8,
            EGL14.EGL_GREEN_SIZE, 8,
            EGL14.EGL_BLUE_SIZE, 8,
            EGL14.EGL_ALPHA_SIZE, 8,
            EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
            EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT,
            EGL_RECORDABLE_ANDROID, 1,
            EGL14.EGL_NONE
        )

        val configs = arrayOfNulls<EGLConfig>(1)
        val numConfig = IntArray(1)
        if (!EGL14.eglChooseConfig(eglDisplay, configAttribs, 0, configs, 0, 1, numConfig, 0)) {
            throw RuntimeException("eglChooseConfig failed")
        }
        eglConfig = configs[0]

        val contextAttribs = intArrayOf(
            EGL14.EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL14.EGL_NONE
        )

        eglContext = EGL14.eglCreateContext(eglDisplay, eglConfig, EGL14.EGL_NO_CONTEXT, contextAttribs, 0)
        if (eglContext == EGL14.EGL_NO_CONTEXT) throw RuntimeException("eglCreateContext failed")

        // Make current on a pbuffer temporarily; window surfaces are created later.
        val pbufAttribs = intArrayOf(EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE)
        val pbuf = EGL14.eglCreatePbufferSurface(eglDisplay, eglConfig, pbufAttribs, 0)
        if (!EGL14.eglMakeCurrent(eglDisplay, pbuf, pbuf, eglContext)) {
            throw RuntimeException("eglMakeCurrent(pbuffer) failed")
        }

        EGL14.eglSwapInterval(eglDisplay, 0)
        Log.i(TAG, "EGL initialized")
    }

    private fun initGl() {
        val vertexShader = compileShader(GLES20.GL_VERTEX_SHADER, VERTEX_SHADER)
        val fragmentShader = compileShader(GLES20.GL_FRAGMENT_SHADER, FRAGMENT_SHADER)

        program = GLES20.glCreateProgram()
        GLES20.glAttachShader(program, vertexShader)
        GLES20.glAttachShader(program, fragmentShader)
        GLES20.glLinkProgram(program)

        val linkStatus = IntArray(1)
        GLES20.glGetProgramiv(program, GLES20.GL_LINK_STATUS, linkStatus, 0)
        if (linkStatus[0] == 0) {
            val log = GLES20.glGetProgramInfoLog(program)
            GLES20.glDeleteProgram(program)
            throw RuntimeException("Program link failed: $log")
        }

        aPositionLoc = GLES20.glGetAttribLocation(program, "aPosition")
        aTexCoordLoc = GLES20.glGetAttribLocation(program, "aTexCoord")
        uTextureLoc = GLES20.glGetUniformLocation(program, "uTexture")
        uTexMatrixLoc = GLES20.glGetUniformLocation(program, "uTexMatrix")

        val texIds = IntArray(1)
        GLES20.glGenTextures(1, texIds, 0)
        texId = texIds[0]

        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texId)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)

        vertexBuffer = ByteBuffer.allocateDirect(QUAD_VERTICES.size * 4)
            .order(ByteOrder.nativeOrder()).asFloatBuffer()
            .put(QUAD_VERTICES).also { it.position(0) }

        texCoordBuffer = ByteBuffer.allocateDirect(8 * 4)
            .order(ByteOrder.nativeOrder()).asFloatBuffer()

        android.opengl.Matrix.setIdentityM(texMatrix, 0)

        Log.i(TAG, "GL initialized")
    }

    private fun createSurfaceTexture() {
        surfaceTexture = SurfaceTexture(texId).apply {
            setDefaultBufferSize(targetW, targetH)
            setOnFrameAvailableListener { hasNewFrame = true }
        }
        inputSurface = Surface(surfaceTexture)
        Log.i(TAG, "SurfaceTexture created: ${targetW}x${targetH}")
    }

    private fun createOutputSurfaces() {
        eglOuts.clear()
        val surfaceAttribs = intArrayOf(EGL14.EGL_NONE)
        for (o in outputs) {
            val es = EGL14.eglCreateWindowSurface(eglDisplay, eglConfig, o.outputSurface, surfaceAttribs, 0)
            if (es == EGL14.EGL_NO_SURFACE) {
                Log.e(TAG, "eglCreateWindowSurface failed tile=${o.tileIndex}")
                continue
            }
            eglOuts.add(OutEgl(o, es))
        }
        Log.i(TAG, "Created ${eglOuts.size} output EGL surfaces")
    }

    private fun drawFrame() {
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
        GLES20.glUseProgram(program)

        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texId)
        GLES20.glUniform1i(uTextureLoc, 0)

        GLES20.glUniformMatrix4fv(uTexMatrixLoc, 1, false, texMatrix, 0)

        GLES20.glEnableVertexAttribArray(aPositionLoc)
        GLES20.glVertexAttribPointer(aPositionLoc, 2, GLES20.GL_FLOAT, false, 0, vertexBuffer)

        GLES20.glEnableVertexAttribArray(aTexCoordLoc)
        GLES20.glVertexAttribPointer(aTexCoordLoc, 2, GLES20.GL_FLOAT, false, 0, texCoordBuffer)

        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)

        GLES20.glDisableVertexAttribArray(aPositionLoc)
        GLES20.glDisableVertexAttribArray(aTexCoordLoc)
    }

    private fun compileShader(type: Int, source: String): Int {
        val shader = GLES20.glCreateShader(type)
        GLES20.glShaderSource(shader, source)
        GLES20.glCompileShader(shader)

        val status = IntArray(1)
        GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, status, 0)
        if (status[0] == 0) {
            val log = GLES20.glGetShaderInfoLog(shader)
            GLES20.glDeleteShader(shader)
            throw RuntimeException("Shader compile failed: $log")
        }
        return shader
    }

    private fun releaseGl() {
        if (program != 0) {
            GLES20.glDeleteProgram(program)
            program = 0
        }
        if (texId != 0) {
            GLES20.glDeleteTextures(1, intArrayOf(texId), 0)
            texId = 0
        }
        inputSurface?.release()
        inputSurface = null
        surfaceTexture?.release()
        surfaceTexture = null
    }

    private fun releaseEgl() {
        if (eglDisplay != EGL14.EGL_NO_DISPLAY) {
            for (oe in eglOuts) {
                if (oe.eglSurface != EGL14.EGL_NO_SURFACE) {
                    EGL14.eglDestroySurface(eglDisplay, oe.eglSurface)
                }
            }
            EGL14.eglMakeCurrent(eglDisplay, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
            if (eglContext != EGL14.EGL_NO_CONTEXT) {
                EGL14.eglDestroyContext(eglDisplay, eglContext)
            }
            EGL14.eglTerminate(eglDisplay)
        }
        eglDisplay = EGL14.EGL_NO_DISPLAY
        eglContext = EGL14.EGL_NO_CONTEXT
        eglOuts.clear()
    }
}
