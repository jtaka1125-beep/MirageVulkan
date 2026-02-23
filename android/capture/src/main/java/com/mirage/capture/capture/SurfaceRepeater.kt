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
 * EGL-based surface repeater v3: matched to MirageComplete's working config.
 *
 * Key differences from v1 that caused frame freeze:
 * - Removed EGL_RECORDABLE_ANDROID (not present in working MirageComplete version)
 * - Removed eglSwapInterval(0)
 * - Using simple Thread.sleep frame pacing (matching MirageComplete)
 * - Added wait/notify for reliable frame wakeup
 */
class SurfaceRepeater(
    private val outputSurface: Surface,
    private val width: Int,
    private val height: Int,
    @Volatile private var targetFps: Int = 30
) {
    companion object {
        private const val TAG = "SurfaceRepeater"

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
                vec2 transformedCoord = (uTexMatrix * vec4(vTexCoord, 0.0, 1.0)).xy;
                gl_FragColor = texture2D(uTexture, transformedCoord);
            }
        """

        private val QUAD_VERTICES = floatArrayOf(
            -1f, -1f,  1f, -1f,  -1f,  1f,  1f,  1f
        )

        private val TEX_COORDS = floatArrayOf(
            0f, 0f,  1f, 0f,  0f, 1f,  1f, 1f
        )
    }

    private var eglDisplay: EGLDisplay = EGL14.EGL_NO_DISPLAY
    private var eglContext: EGLContext = EGL14.EGL_NO_CONTEXT
    private var eglSurface: EGLSurface = EGL14.EGL_NO_SURFACE

    private var program = 0
    private var texId = 0
    private var aPositionLoc = 0
    private var aTexCoordLoc = 0
    private var uTextureLoc = 0
    private var uTexMatrixLoc = 0

    private var surfaceTexture: SurfaceTexture? = null
    var inputSurface: Surface? = null
        private set

    @Volatile private var running = false
    @Volatile private var hasNewFrame = false
    private var renderThread: Thread? = null
    private val texMatrix = FloatArray(16)

    private lateinit var vertexBuffer: FloatBuffer
    private lateinit var texCoordBuffer: FloatBuffer

    fun start() {
        running = true
        renderThread = Thread({ renderLoop() }, "SurfaceRepeater").also { it.start() }

        val deadline = System.currentTimeMillis() + 3000
        while (inputSurface == null && System.currentTimeMillis() < deadline) {
            Thread.sleep(10)
        }

        if (inputSurface == null) {
            Log.e(TAG, "Failed to create inputSurface within timeout")
            stop()
        } else {
            Log.i(TAG, "Started v3: ${width}x${height} @ ${targetFps}fps")
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

            var frameIntervalMs = 1000L / targetFps
            var frameCount = 0L
            var lastLogTime = System.currentTimeMillis()

            while (running) {
                val frameStart = System.currentTimeMillis()

                if (hasNewFrame) {
                    surfaceTexture?.updateTexImage()
                    surfaceTexture?.getTransformMatrix(texMatrix)
                    hasNewFrame = false
                }

                drawFrame()
                EGL14.eglSwapBuffers(eglDisplay, eglSurface)
                frameCount++

                val now = System.currentTimeMillis()
                if (now - lastLogTime >= 5000) {
                    val elapsed = (now - lastLogTime) / 1000.0
                    val fps = frameCount / elapsed
                    Log.i(TAG, "Repeater: %.1f fps (%d frames in %.1fs)".format(fps, frameCount, elapsed))
                    frameCount = 0
                    lastLogTime = now
                }

                frameIntervalMs = 1000L / targetFps
                val elapsed = System.currentTimeMillis() - frameStart
                val sleepMs = frameIntervalMs - elapsed
                if (sleepMs > 0) {
                    Thread.sleep(sleepMs)
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
        if (!EGL14.eglInitialize(eglDisplay, version, 0, version, 1))
            throw RuntimeException("eglInitialize failed")

        // Match MirageComplete config: NO EGL_RECORDABLE_ANDROID
        val configAttribs = intArrayOf(
            EGL14.EGL_RED_SIZE, 8,
            EGL14.EGL_GREEN_SIZE, 8,
            EGL14.EGL_BLUE_SIZE, 8,
            EGL14.EGL_ALPHA_SIZE, 8,
            EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
            EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT,
            EGL14.EGL_NONE
        )

        val configs = arrayOfNulls<EGLConfig>(1)
        val numConfig = IntArray(1)
        if (!EGL14.eglChooseConfig(eglDisplay, configAttribs, 0, configs, 0, 1, numConfig, 0))
            throw RuntimeException("eglChooseConfig failed")

        val contextAttribs = intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE)
        eglContext = EGL14.eglCreateContext(eglDisplay, configs[0], EGL14.EGL_NO_CONTEXT, contextAttribs, 0)
        if (eglContext == EGL14.EGL_NO_CONTEXT) throw RuntimeException("eglCreateContext failed")

        val surfaceAttribs = intArrayOf(EGL14.EGL_NONE)
        eglSurface = EGL14.eglCreateWindowSurface(eglDisplay, configs[0], outputSurface, surfaceAttribs, 0)
        if (eglSurface == EGL14.EGL_NO_SURFACE) throw RuntimeException("eglCreateWindowSurface failed")

        if (!EGL14.eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext))
            throw RuntimeException("eglMakeCurrent failed")

        // NO eglSwapInterval(0) - match MirageComplete
        Log.i(TAG, "EGL initialized (v3, no RECORDABLE): version ${version[0]}.${version[1]}")
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
        texCoordBuffer = ByteBuffer.allocateDirect(TEX_COORDS.size * 4)
            .order(ByteOrder.nativeOrder()).asFloatBuffer()
            .put(TEX_COORDS).also { it.position(0) }

        android.opengl.Matrix.setIdentityM(texMatrix, 0)
        GLES20.glViewport(0, 0, width, height)
        Log.i(TAG, "GL initialized: texture=$texId")
    }

    private fun createSurfaceTexture() {
        surfaceTexture = SurfaceTexture(texId).apply {
            setDefaultBufferSize(width, height)
            setOnFrameAvailableListener { hasNewFrame = true }
        }
        inputSurface = Surface(surfaceTexture)
        Log.i(TAG, "SurfaceTexture created: ${width}x${height}")
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
        if (program != 0) { GLES20.glDeleteProgram(program); program = 0 }
        if (texId != 0) { GLES20.glDeleteTextures(1, intArrayOf(texId), 0); texId = 0 }
        inputSurface?.release(); inputSurface = null
        surfaceTexture?.release(); surfaceTexture = null
    }

    private fun releaseEgl() {
        if (eglDisplay != EGL14.EGL_NO_DISPLAY) {
            EGL14.eglMakeCurrent(eglDisplay, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
            if (eglSurface != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(eglDisplay, eglSurface)
            if (eglContext != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(eglDisplay, eglContext)
            EGL14.eglTerminate(eglDisplay)
        }
        eglDisplay = EGL14.EGL_NO_DISPLAY
        eglContext = EGL14.EGL_NO_CONTEXT
        eglSurface = EGL14.EGL_NO_SURFACE
    }
}
