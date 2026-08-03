package com.archstreamer.client.ui

import android.content.Context
import android.graphics.SurfaceTexture
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.opengl.GLSurfaceView
import android.opengl.Matrix
import android.os.Handler
import android.os.Looper
import android.view.Surface
import android.view.ViewGroup
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/**
 * Decodes a melonDS Hybrid stream and presents large/small panes stacked for portrait.
 *
 * Hybrid ratio R (we use 3): buffer is roughly `(R·256 + 256) × (R·192)`.
 * Large DS screen is the left `R/(R+1…)` of the frame; small screen is the right
 * column, bottom-aligned (`melonds_hybrid_small_screen = Bottom`).
 *
 * UVs follow the Android SurfaceTexture convention: pass coords as if (0,0) is the
 * bottom-left of the buffer, then multiply by [getTransformMatrix] in the shader.
 */
class DsHybridPortraitView(context: Context) : GLSurfaceView(context), GLSurfaceView.Renderer {
    /** melonDS hybrid_ratio (ArchStreamer writes 3). */
    var hybridRatio: Float = 3f

    @Volatile
    var streamAspect: Float = 16f / 9f

    @Volatile
    private var frameAvailable = false

    private var surfaceTexture: SurfaceTexture? = null
    private var decoderSurface: Surface? = null
    private var oesTextureId = 0
    private var program = 0
    private var aPos = -1
    private var aUv = -1
    private var uTex = -1
    private var uTexMatrix = -1
    private var surfaceW = 1
    private var surfaceH = 1

    private val mainHandler = Handler(Looper.getMainLooper())
    private val vertexBuf: FloatBuffer =
        ByteBuffer.allocateDirect(4 * 2 * 4).order(ByteOrder.nativeOrder()).asFloatBuffer()
    private val uvBuf: FloatBuffer =
        ByteBuffer.allocateDirect(4 * 2 * 4).order(ByteOrder.nativeOrder()).asFloatBuffer()
    private val texMatrix = FloatArray(16)

    var onSurfaceReady: ((Surface) -> Unit)? = null
    var onSurfaceDestroyed: (() -> Unit)? = null

    /** Live decoder target, if GL has created it (for player rebind without recreate). */
    fun currentDecoderSurface(): Surface? = decoderSurface

    init {
        layoutParams = ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT,
        )
        setEGLContextClientVersion(2)
        setRenderer(this)
        // Continuous: Hybrid crops stay live even if a frame-available callback is delayed.
        renderMode = RENDERMODE_CONTINUOUSLY
        keepScreenOn = true
        Matrix.setIdentityM(texMatrix, 0)
    }

    override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
        oesTextureId = genOesTexture()
        surfaceTexture = SurfaceTexture(oesTextureId).also { st ->
            st.setOnFrameAvailableListener(
                {
                    frameAvailable = true
                },
                mainHandler,
            )
        }
        decoderSurface = Surface(surfaceTexture)
        program = buildProgram(VERT, FRAG)
        aPos = GLES20.glGetAttribLocation(program, "aPos")
        aUv = GLES20.glGetAttribLocation(program, "aUv")
        uTex = GLES20.glGetUniformLocation(program, "uTex")
        uTexMatrix = GLES20.glGetUniformLocation(program, "uTexMatrix")
        GLES20.glClearColor(0f, 0f, 0f, 1f)
        post { decoderSurface?.let { onSurfaceReady?.invoke(it) } }
    }

    override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
        surfaceW = width.coerceAtLeast(1)
        surfaceH = height.coerceAtLeast(1)
        GLES20.glViewport(0, 0, surfaceW, surfaceH)
    }

    override fun onDrawFrame(gl: GL10?) {
        if (frameAvailable) {
            frameAvailable = false
            runCatching {
                surfaceTexture?.updateTexImage()
                surfaceTexture?.getTransformMatrix(texMatrix)
            }
        }
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
        if (program == 0 || surfaceTexture == null) return

        val viewW = surfaceW.toFloat()
        val viewH = surfaceH.toFloat()
        val r = hybridRatio.coerceIn(2f, 4f)
        // melonDS: width = R*256 + 256 + 2*R, height = R*192
        val nativeW = 256f
        val primaryW = nativeW * r
        val bufferW = primaryW + nativeW + 2f * r
        val split = (primaryW / bufferW).coerceIn(0.5f, 0.9f)
        // Small screen is native 192 tall in a R*192 buffer, bottom-aligned.
        val smallHeightFrac = (1f / r).coerceIn(0.15f, 0.5f)

        // Two 4:3 panes, full width, stacked and vertically centered.
        val paneH = viewW * 0.75f
        val totalH = paneH * 2f
        val originY = ((viewH - totalH) * 0.5f).coerceAtLeast(0f)
        val usedPaneH = if (totalH > viewH) viewH * 0.5f else paneH

        // Image-space crops: (0,0)=top-left of Hybrid frame, (1,1)=bottom-right.
        // Top phone pane ← large (left) screen.
        drawPane(
            drawX = 0f,
            drawY = originY,
            drawW = viewW,
            drawH = usedPaneH,
            imageLeft = 0f,
            imageRight = split,
            imageTop = 0f,
            imageBottom = 1f,
        )
        // Bottom phone pane ← small (right, bottom-aligned) screen.
        drawPane(
            drawX = 0f,
            drawY = originY + usedPaneH,
            drawW = viewW,
            drawH = usedPaneH,
            imageLeft = split,
            imageRight = (primaryW + nativeW) / bufferW,
            imageTop = 1f - smallHeightFrac,
            imageBottom = 1f,
        )
    }

    /**
     * @param imageTop/imageBottom top-left-origin fractions of the Hybrid frame
     *   (0 = top of video, 1 = bottom of video).
     */
    private fun drawPane(
        drawX: Float,
        drawY: Float,
        drawW: Float,
        drawH: Float,
        imageLeft: Float,
        imageRight: Float,
        imageTop: Float,
        imageBottom: Float,
    ) {
        val viewW = surfaceW.toFloat()
        val viewH = surfaceH.toFloat()

        fun toNdcX(px: Float) = (px / viewW) * 2f - 1f
        fun toNdcY(py: Float) = 1f - (py / viewH) * 2f

        val x0 = toNdcX(drawX)
        val x1 = toNdcX(drawX + drawW)
        val yTop = toNdcY(drawY)
        val yBot = toNdcY(drawY + drawH)

        // Convert top-left image UVs → SurfaceTexture UV space (0,0 = bottom-left).
        val u0 = imageLeft
        val u1 = imageRight
        val vBottom = 1f - imageBottom // ST v for image bottom edge
        val vTop = 1f - imageTop // ST v for image top edge

        vertexBuf.rewind()
        vertexBuf.put(
            floatArrayOf(
                x0, yBot,
                x1, yBot,
                x0, yTop,
                x1, yTop,
            ),
        )
        vertexBuf.rewind()

        // Triangle strip BL, BR, TL, TR — ST uvs match working full-screen blit convention.
        uvBuf.rewind()
        uvBuf.put(
            floatArrayOf(
                u0, vBottom,
                u1, vBottom,
                u0, vTop,
                u1, vTop,
            ),
        )
        uvBuf.rewind()

        GLES20.glUseProgram(program)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTextureId)
        GLES20.glUniform1i(uTex, 0)
        GLES20.glUniformMatrix4fv(uTexMatrix, 1, false, texMatrix, 0)

        GLES20.glEnableVertexAttribArray(aPos)
        GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 0, vertexBuf)
        GLES20.glEnableVertexAttribArray(aUv)
        GLES20.glVertexAttribPointer(aUv, 2, GLES20.GL_FLOAT, false, 0, uvBuf)
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
        GLES20.glDisableVertexAttribArray(aPos)
        GLES20.glDisableVertexAttribArray(aUv)
    }

    override fun onDetachedFromWindow() {
        onSurfaceDestroyed?.invoke()
        queueEvent {
            runCatching { decoderSurface?.release() }
            runCatching { surfaceTexture?.release() }
            decoderSurface = null
            surfaceTexture = null
            if (oesTextureId != 0) {
                GLES20.glDeleteTextures(1, intArrayOf(oesTextureId), 0)
                oesTextureId = 0
            }
            if (program != 0) {
                GLES20.glDeleteProgram(program)
                program = 0
            }
        }
        super.onDetachedFromWindow()
    }

    companion object {
        private const val VERT = """
            attribute vec2 aPos;
            attribute vec2 aUv;
            uniform mat4 uTexMatrix;
            varying vec2 vUv;
            void main() {
                vUv = (uTexMatrix * vec4(aUv, 0.0, 1.0)).xy;
                gl_Position = vec4(aPos, 0.0, 1.0);
            }
        """

        private const val FRAG = """
            #extension GL_OES_EGL_image_external : require
            precision mediump float;
            varying vec2 vUv;
            uniform samplerExternalOES uTex;
            void main() {
                gl_FragColor = texture2D(uTex, vUv);
            }
        """

        private fun genOesTexture(): Int {
            val ids = IntArray(1)
            GLES20.glGenTextures(1, ids, 0)
            val id = ids[0]
            GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, id)
            GLES20.glTexParameteri(
                GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
                GLES20.GL_TEXTURE_MIN_FILTER,
                GLES20.GL_LINEAR,
            )
            GLES20.glTexParameteri(
                GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
                GLES20.GL_TEXTURE_MAG_FILTER,
                GLES20.GL_LINEAR,
            )
            GLES20.glTexParameteri(
                GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
                GLES20.GL_TEXTURE_WRAP_S,
                GLES20.GL_CLAMP_TO_EDGE,
            )
            GLES20.glTexParameteri(
                GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
                GLES20.GL_TEXTURE_WRAP_T,
                GLES20.GL_CLAMP_TO_EDGE,
            )
            return id
        }

        private fun buildProgram(vertSrc: String, fragSrc: String): Int {
            val vs = compile(GLES20.GL_VERTEX_SHADER, vertSrc)
            val fs = compile(GLES20.GL_FRAGMENT_SHADER, fragSrc)
            val prog = GLES20.glCreateProgram()
            GLES20.glAttachShader(prog, vs)
            GLES20.glAttachShader(prog, fs)
            GLES20.glLinkProgram(prog)
            val link = IntArray(1)
            GLES20.glGetProgramiv(prog, GLES20.GL_LINK_STATUS, link, 0)
            if (link[0] == 0) {
                val log = GLES20.glGetProgramInfoLog(prog)
                GLES20.glDeleteProgram(prog)
                error("GL link failed: $log")
            }
            GLES20.glDeleteShader(vs)
            GLES20.glDeleteShader(fs)
            return prog
        }

        private fun compile(type: Int, src: String): Int {
            val shader = GLES20.glCreateShader(type)
            GLES20.glShaderSource(shader, src)
            GLES20.glCompileShader(shader)
            val ok = IntArray(1)
            GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, ok, 0)
            if (ok[0] == 0) {
                val log = GLES20.glGetShaderInfoLog(shader)
                GLES20.glDeleteShader(shader)
                error("GL compile failed: $log")
            }
            return shader
        }
    }
}
