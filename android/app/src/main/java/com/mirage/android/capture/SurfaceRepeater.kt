package com.mirage.android.capture

import android.graphics.SurfaceTexture
import android.opengl.*
import android.opengl.GLES11Ext
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer

/**
 * EGL-based surface repeater that ensures consistent frame rate
 * even when the source (VirtualDisplay/MediaProjection) produces
 * frames infrequently on static screens.
 *
 * Architecture:
 *   VirtualDisplay → [inputSurface] → SurfaceTexture(GL) 
 *       → periodic redraw at targetFps → [outputSurface (encoder)]
 *
 * This solves the MediaTek (and other) SoC issue where 
 * KEY_REPEAT_PREVIOUS_FRAME_AFTER is ignored by the hardware encoder.
 */
class SurfaceRepeater(
    private val outputSurface: Surface,
    private val width: Int,
    private val height: Int,
    @Volatile private var targetFps: Int = 30
) {
    companion object {
        private const val TAG = "SurfaceRepeater"

        // Vertex shader - simple passthrough
        private const val VERTEX_SHADER = """
            attribute vec4 aPosition;
            attribute vec2 aTexCoord;
            varying vec2 vTexCoord;
            void main() {
                gl_Position = aPosition;
                vTexCoord = aTexCoord;
            }
        """

        // Fragment shader for external OES texture (from SurfaceTexture)
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

        // Full-screen quad vertices
        private val QUAD_VERTICES = floatArrayOf(
            -1f, -1f,  // bottom-left
             1f, -1f,  // bottom-right
            -1f,  1f,  // top-left
             1f,  1f   // top-right
        )

        // Texture coordinates
        private val TEX_COORDS = floatArrayOf(
            0f, 0f,
            1f, 0f,
            0f, 1f,
            1f, 1f
        )
    }

    // EGL objects
    private var eglDisplay: EGLDisplay = EGL14.EGL_NO_DISPLAY
    private var eglContext: EGLContext = EGL14.EGL_NO_CONTEXT
    private var eglSurface: EGLSurface = EGL14.EGL_NO_SURFACE

    // GL objects
    private var program = 0
    private var texId = 0
    private var aPositionLoc = 0
    private var aTexCoordLoc = 0
    private var uTextureLoc = 0
    private var uTexMatrixLoc = 0

    // SurfaceTexture (receives frames from VirtualDisplay)
    private var surfaceTexture: SurfaceTexture? = null
    
    /** Surface that VirtualDisplay should render to */
    var inputSurface: Surface? = null
        private set

    // State
    @Volatile private var running = false
    @Volatile private var hasNewFrame = false
    private var renderThread: Thread? = null
    private val texMatrix = FloatArray(16)

    // Vertex/TexCoord buffers
    private lateinit var vertexBuffer: FloatBuffer
    private lateinit var texCoordBuffer: FloatBuffer

    /**
     * Start the repeater. Creates EGL context, SurfaceTexture, and render thread.
     * Must be called before using inputSurface.
     */
    fun start() {
        running = true
        renderThread = Thread({ renderLoop() }, "SurfaceRepeater").also { it.start() }

        // Wait for inputSurface to be ready (created on render thread)
        val deadline = System.currentTimeMillis() + 3000
        while (inputSurface == null && System.currentTimeMillis() < deadline) {
            Thread.sleep(10)
        }

        if (inputSurface == null) {
            Log.e(TAG, "Failed to create inputSurface within timeout")
            stop()
        } else {
            Log.i(TAG, "Started: ${width}x${height} @ ${targetFps}fps")
        }
    }

    /**
     * Stop and release all resources.
     */
    fun stop() {
        running = false
        renderThread?.join(2000)
        renderThread = null
        Log.i(TAG, "Stopped")
    }

    /**
     * Dynamically update the target FPS.
     * Takes effect on the next frame interval calculation.
     */
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

                // Update texture if new frame available
                if (hasNewFrame) {
                    surfaceTexture?.updateTexImage()
                    surfaceTexture?.getTransformMatrix(texMatrix)
                    hasNewFrame = false
                }

                // Draw to encoder surface (even if no new frame - this is the key!)
                drawFrame()

                // Swap buffers to push frame to encoder
                EGL14.eglSwapBuffers(eglDisplay, eglSurface)

                frameCount++

                // Stats logging every 5 seconds
                val now = System.currentTimeMillis()
                if (now - lastLogTime >= 5000) {
                    val elapsed = (now - lastLogTime) / 1000.0
                    val fps = frameCount / elapsed
                    Log.i(TAG, "Repeater: %.1f fps (%d frames in %.1fs)".format(fps, frameCount, elapsed))
                    frameCount = 0
                    lastLogTime = now
                }

                // Frame pacing (re-read targetFps each iteration for dynamic updates)
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
        if (eglDisplay == EGL14.EGL_NO_DISPLAY) {
            throw RuntimeException("eglGetDisplay failed")
        }

        val version = IntArray(2)
        if (!EGL14.eglInitialize(eglDisplay, version, 0, version, 1)) {
            throw RuntimeException("eglInitialize failed")
        }

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
        if (!EGL14.eglChooseConfig(eglDisplay, configAttribs, 0, configs, 0, 1, numConfig, 0)) {
            throw RuntimeException("eglChooseConfig failed")
        }

        val contextAttribs = intArrayOf(
            EGL14.EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL14.EGL_NONE
        )

        eglContext = EGL14.eglCreateContext(
            eglDisplay, configs[0], EGL14.EGL_NO_CONTEXT, contextAttribs, 0
        )
        if (eglContext == EGL14.EGL_NO_CONTEXT) {
            throw RuntimeException("eglCreateContext failed")
        }

        // Create window surface from encoder's input surface
        val surfaceAttribs = intArrayOf(EGL14.EGL_NONE)
        eglSurface = EGL14.eglCreateWindowSurface(
            eglDisplay, configs[0], outputSurface, surfaceAttribs, 0
        )
        if (eglSurface == EGL14.EGL_NO_SURFACE) {
            throw RuntimeException("eglCreateWindowSurface failed")
        }

        if (!EGL14.eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext)) {
            throw RuntimeException("eglMakeCurrent failed")
        }

        Log.i(TAG, "EGL initialized: version ${version[0]}.${version[1]}")
    }

    private fun initGl() {
        // Compile shaders
        val vertexShader = compileShader(GLES20.GL_VERTEX_SHADER, VERTEX_SHADER)
        val fragmentShader = compileShader(GLES20.GL_FRAGMENT_SHADER, FRAGMENT_SHADER)

        // Link program
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

        // Get attribute/uniform locations
        aPositionLoc = GLES20.glGetAttribLocation(program, "aPosition")
        aTexCoordLoc = GLES20.glGetAttribLocation(program, "aTexCoord")
        uTextureLoc = GLES20.glGetUniformLocation(program, "uTexture")
        uTexMatrixLoc = GLES20.glGetUniformLocation(program, "uTexMatrix")

        // Create external OES texture
        val texIds = IntArray(1)
        GLES20.glGenTextures(1, texIds, 0)
        texId = texIds[0]

        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texId)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
            GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
            GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
            GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
            GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)

        // Prepare vertex buffers
        vertexBuffer = ByteBuffer.allocateDirect(QUAD_VERTICES.size * 4)
            .order(ByteOrder.nativeOrder()).asFloatBuffer()
            .put(QUAD_VERTICES).also { it.position(0) }

        texCoordBuffer = ByteBuffer.allocateDirect(TEX_COORDS.size * 4)
            .order(ByteOrder.nativeOrder()).asFloatBuffer()
            .put(TEX_COORDS).also { it.position(0) }

        // Initialize texMatrix to identity
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

        // Bind texture
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texId)
        GLES20.glUniform1i(uTextureLoc, 0)

        // Set texture transform matrix
        GLES20.glUniformMatrix4fv(uTexMatrixLoc, 1, false, texMatrix, 0)

        // Set vertex positions
        GLES20.glEnableVertexAttribArray(aPositionLoc)
        GLES20.glVertexAttribPointer(aPositionLoc, 2, GLES20.GL_FLOAT, false, 0, vertexBuffer)

        // Set texture coordinates
        GLES20.glEnableVertexAttribArray(aTexCoordLoc)
        GLES20.glVertexAttribPointer(aTexCoordLoc, 2, GLES20.GL_FLOAT, false, 0, texCoordBuffer)

        // Draw
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
            EGL14.eglMakeCurrent(eglDisplay, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
            if (eglSurface != EGL14.EGL_NO_SURFACE) {
                EGL14.eglDestroySurface(eglDisplay, eglSurface)
            }
            if (eglContext != EGL14.EGL_NO_CONTEXT) {
                EGL14.eglDestroyContext(eglDisplay, eglContext)
            }
            EGL14.eglTerminate(eglDisplay)
        }
        eglDisplay = EGL14.EGL_NO_DISPLAY
        eglContext = EGL14.EGL_NO_CONTEXT
        eglSurface = EGL14.EGL_NO_SURFACE
    }
}
