package com.mirage.capture.capture

import android.graphics.SurfaceTexture
import android.os.SystemClock
import android.util.Log
import android.view.Surface
import javax.microedition.khronos.egl.EGL10

/**
 * VirtualDisplay -> SurfaceTexture(OES) -> (repeat @ targetFps) -> MediaCodec input surface.
 * This breaks the 30fps supply ceiling by swapping the encoder surface at target fps,
 * reusing the latest texture when no new frame arrives.
 */
class SurfaceRepeater(
    private val width: Int,
    private val height: Int,
    private val targetFps: Int,
    private val encoderSurface: Surface,
    private val onSurfaceReady: (Surface) -> Unit,
) {
    private val TAG = "MirageRepeater"

    @Volatile private var running = false
    private var thread: Thread? = null

    private var surfaceTexture: SurfaceTexture? = null
    private var sourceSurface: Surface? = null

    fun start() {
        if (running) return
        running = true
        thread = Thread({ runLoop() }, "mirage-repeater")
        thread!!.start()
    }

    fun stop() {
        running = false
        try { thread?.join(1500) } catch (_: Exception) {}
        try { sourceSurface?.release() } catch (_: Exception) {}
        try { surfaceTexture?.release() } catch (_: Exception) {}
        sourceSurface = null
        surfaceTexture = null
        thread = null
    }

    private fun runLoop() {
        // Use legacy EGL10 for minimal deps
        val egl = javax.microedition.khronos.egl.EGLContext.getEGL() as EGL10
        val display = egl.eglGetDisplay(EGL10.EGL_DEFAULT_DISPLAY)
        val version = IntArray(2)
        egl.eglInitialize(display, version)

        val configSpec = intArrayOf(
            EGL10.EGL_RENDERABLE_TYPE, 4, // EGL_OPENGL_ES2_BIT
            EGL10.EGL_RED_SIZE, 8,
            EGL10.EGL_GREEN_SIZE, 8,
            EGL10.EGL_BLUE_SIZE, 8,
            EGL10.EGL_ALPHA_SIZE, 8,
            EGL10.EGL_NONE
        )
        val configs = arrayOfNulls<javax.microedition.khronos.egl.EGLConfig>(1)
        val num = IntArray(1)
        egl.eglChooseConfig(display, configSpec, configs, 1, num)
        val config = configs[0]!!

        val attribList = intArrayOf(0x3098, 2, EGL10.EGL_NONE) // EGL_CONTEXT_CLIENT_VERSION=2
        val context = egl.eglCreateContext(display, config, EGL10.EGL_NO_CONTEXT, attribList)

        val winAttribs = intArrayOf(EGL10.EGL_NONE)
        val eglSurface = egl.eglCreateWindowSurface(display, config, encoderSurface, winAttribs)
        egl.eglMakeCurrent(display, eglSurface, eglSurface, context)

        // GL setup
        val texId = GlUtil.createExternalTexture()
        surfaceTexture = SurfaceTexture(texId)
        surfaceTexture!!.setDefaultBufferSize(width, height)
        val ss = Surface(surfaceTexture)
        sourceSurface = ss
        onSurfaceReady(ss)

        val program = GlUtil.createOesProgram()
        val quad = GlUtil.fullscreenQuad()

        val intervalNs = (1_000_000_000L / targetFps.toLong()).coerceAtLeast(10_000_000L)
        Log.i(TAG, "SurfaceRepeater start target=$targetFps intervalNs=$intervalNs")

        var nextNs = System.nanoTime()
        while (running) {
            val nowNs = System.nanoTime()
            val sleepNs = nextNs - nowNs
            if (sleepNs > 0) {
                // Sleep in ms chunks, then yield
                SystemClock.sleep((sleepNs / 1_000_000L).coerceAtMost(10L))
                continue
            }
            // If we're far behind, resync
            if (-sleepNs > intervalNs * 5) {
                nextNs = nowNs
            }
            nextNs += intervalNs

            try {
                surfaceTexture!!.updateTexImage()
            } catch (_: Exception) {
                // ignore when no new frame; we still draw last texture
            }

            GlUtil.drawOes(program, quad, texId)
            egl.eglSwapBuffers(display, eglSurface)
        }

        // teardown
        try { GlUtil.destroyProgram(program) } catch (_: Exception) {}
        egl.eglMakeCurrent(display, EGL10.EGL_NO_SURFACE, EGL10.EGL_NO_SURFACE, EGL10.EGL_NO_CONTEXT)
        egl.eglDestroySurface(display, eglSurface)
        egl.eglDestroyContext(display, context)
        egl.eglTerminate(display)
        Log.i(TAG, "SurfaceRepeater stopped")
    }
}
