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
 * Decodes a landscape DS stream (Horizontal + EmphTop/EmphBot) into a single
 * MediaCodec SurfaceTexture, then either letterboxes the full frame (landscape)
 * or UV-crops left/right into a stacked portrait presentation.
 *
 * Keep this view alive across orientation changes so the decoder surface is not
 * torn down (avoids multi-second black waits for the next IDR).
 *
 * Host keeps top=left / bottom=right; SwapScreenEmphasis only swaps sizes.
 * UV fractions come from shared [DsTouchMapping.horizontalEmphFracs].
 */
class DsHybridPortraitView(context: Context) : GLSurfaceView(context), GLSurfaceView.Renderer {
    /** Width emphasis ratio matching host EmphTop / hybrid_ratio (ArchStreamer uses 3). */
    var hybridRatio: Float = 3f

    /** When true, bottom (right) is emphasized — left top screen is the small centered one. */
    @Volatile
    var emphBottom: Boolean = false

    /**
     * When true and the stream is wide, stack left→top / right→bottom phone panes.
     * When false, letterbox the full stream frame (landscape DualScreen path).
     */
    @Volatile
    var portraitStack: Boolean = false

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
        // Continuous: EmphTop crops stay live even if a frame-available callback is delayed.
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
        val aspect = if (streamAspect > 0.1f) streamAspect else (16f / 9f)
        val stack = portraitStack && aspect > 1.15f

        if (!stack) {
            // Landscape (or tall stream): letterbox the full frame into the view.
            val viewAspect = viewW / viewH
            val drawW: Float
            val drawH: Float
            if (viewAspect > aspect) {
                drawH = viewH
                drawW = viewH * aspect
            } else {
                drawW = viewW
                drawH = viewW / aspect
            }
            val drawX = (viewW - drawW) * 0.5f
            val drawY = (viewH - drawH) * 0.5f
            drawPane(
                drawX = drawX,
                drawY = drawY,
                drawW = drawW,
                drawH = drawH,
                imageLeft = 0f,
                imageRight = 1f,
                imageTop = 0f,
                imageBottom = 1f,
            )
            return
        }

        val fr = DsTouchMapping.horizontalEmphFracs(hybridRatio, emphBottom)

        // Two 4:3 panes, full width, stacked and vertically centered.
        val paneH = viewW * 0.75f
        val totalH = paneH * 2f
        val originY = ((viewH - totalH) * 0.5f).coerceAtLeast(0f)
        val usedPaneH = if (totalH > viewH) viewH * 0.5f else paneH

        // Top phone pane ← left column (DS top). Bottom phone pane ← right (DS bottom).
        drawPane(
            drawX = 0f,
            drawY = originY,
            drawW = viewW,
            drawH = usedPaneH,
            imageLeft = fr.leftU0,
            imageRight = fr.leftU1,
            imageTop = fr.leftV0,
            imageBottom = fr.leftV1,
        )
        drawPane(
            drawX = 0f,
            drawY = originY + usedPaneH,
            drawW = viewW,
            drawH = usedPaneH,
            imageLeft = fr.rightU0,
            imageRight = fr.rightU1,
            imageTop = fr.rightV0,
            imageBottom = fr.rightV1,
        )
    }

    /**
     * @param imageTop/imageBottom top-left-origin fractions of the stream frame
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
