package com.mirage.capture.capture

import android.opengl.GLES11Ext
import android.opengl.GLES20

object GlUtil {
    data class Quad(val vbo: IntArray, val ibo: IntArray)

    fun createExternalTexture(): Int {
        val tex = IntArray(1)
        GLES20.glGenTextures(1, tex, 0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, tex[0])
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
        return tex[0]
    }

    fun createOesProgram(): Int {
        val vs = """
            attribute vec4 aPos;
            attribute vec2 aTex;
            varying vec2 vTex;
            void main(){ gl_Position=aPos; vTex=aTex; }
        """.trimIndent()
        val fs = """
            #extension GL_OES_EGL_image_external : require
            precision mediump float;
            varying vec2 vTex;
            uniform samplerExternalOES sTex;
            void main(){ gl_FragColor = texture2D(sTex, vTex); }
        """.trimIndent()
        val vsh = compile(GLES20.GL_VERTEX_SHADER, vs)
        val fsh = compile(GLES20.GL_FRAGMENT_SHADER, fs)
        val prog = GLES20.glCreateProgram()
        GLES20.glAttachShader(prog, vsh)
        GLES20.glAttachShader(prog, fsh)
        GLES20.glLinkProgram(prog)
        val link = IntArray(1)
        GLES20.glGetProgramiv(prog, GLES20.GL_LINK_STATUS, link, 0)
        if (link[0] == 0) {
            val log = GLES20.glGetProgramInfoLog(prog)
            GLES20.glDeleteProgram(prog)
            throw RuntimeException("link failed: $log")
        }
        GLES20.glDeleteShader(vsh)
        GLES20.glDeleteShader(fsh)
        return prog
    }

    private fun compile(type: Int, src: String): Int {
        val sh = GLES20.glCreateShader(type)
        GLES20.glShaderSource(sh, src)
        GLES20.glCompileShader(sh)
        val ok = IntArray(1)
        GLES20.glGetShaderiv(sh, GLES20.GL_COMPILE_STATUS, ok, 0)
        if (ok[0] == 0) {
            val log = GLES20.glGetShaderInfoLog(sh)
            GLES20.glDeleteShader(sh)
            throw RuntimeException("compile failed: $log")
        }
        return sh
    }

    fun fullscreenQuad(): Quad {
        // interleaved: x,y,z,w, u,v
        val verts = floatArrayOf(
            -1f, -1f, 0f, 1f, 0f, 1f,
             1f, -1f, 0f, 1f, 1f, 1f,
             1f,  1f, 0f, 1f, 1f, 0f,
            -1f,  1f, 0f, 1f, 0f, 0f,
        )
        val idx = shortArrayOf(0,1,2, 0,2,3)

        val vbo = IntArray(1)
        GLES20.glGenBuffers(1, vbo, 0)
        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, vbo[0])
        val vb = java.nio.ByteBuffer.allocateDirect(verts.size*4).order(java.nio.ByteOrder.nativeOrder()).asFloatBuffer()
        vb.put(verts).position(0)
        GLES20.glBufferData(GLES20.GL_ARRAY_BUFFER, verts.size*4, vb, GLES20.GL_STATIC_DRAW)

        val ibo = IntArray(1)
        GLES20.glGenBuffers(1, ibo, 0)
        GLES20.glBindBuffer(GLES20.GL_ELEMENT_ARRAY_BUFFER, ibo[0])
        val ib = java.nio.ByteBuffer.allocateDirect(idx.size*2).order(java.nio.ByteOrder.nativeOrder()).asShortBuffer()
        ib.put(idx).position(0)
        GLES20.glBufferData(GLES20.GL_ELEMENT_ARRAY_BUFFER, idx.size*2, ib, GLES20.GL_STATIC_DRAW)

        return Quad(vbo, ibo)
    }

    fun drawOes(program: Int, quad: Quad, texId: Int) {
        GLES20.glViewport(0,0,0x7fffffff,0x7fffffff) // viewport overridden by surface size; safe no-op
        GLES20.glUseProgram(program)
        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, quad.vbo[0])
        GLES20.glBindBuffer(GLES20.GL_ELEMENT_ARRAY_BUFFER, quad.ibo[0])

        val aPos = GLES20.glGetAttribLocation(program, "aPos")
        val aTex = GLES20.glGetAttribLocation(program, "aTex")
        val stride = 6*4
        GLES20.glEnableVertexAttribArray(aPos)
        GLES20.glVertexAttribPointer(aPos, 4, GLES20.GL_FLOAT, false, stride, 0)
        GLES20.glEnableVertexAttribArray(aTex)
        GLES20.glVertexAttribPointer(aTex, 2, GLES20.GL_FLOAT, false, stride, 4*4)

        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texId)
        val u = GLES20.glGetUniformLocation(program, "sTex")
        GLES20.glUniform1i(u, 0)

        GLES20.glDrawElements(GLES20.GL_TRIANGLES, 6, GLES20.GL_UNSIGNED_SHORT, 0)

        GLES20.glDisableVertexAttribArray(aPos)
        GLES20.glDisableVertexAttribArray(aTex)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, 0)
        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, 0)
        GLES20.glBindBuffer(GLES20.GL_ELEMENT_ARRAY_BUFFER, 0)
    }

    fun destroyProgram(program: Int) {
        GLES20.glDeleteProgram(program)
    }
}
