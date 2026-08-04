package com.archstreamer.client.ui

import com.archstreamer.client.protocol.DsScreenLayout

/**
 * Thin JNI façade over include/common/ds_touch_mapping.hpp.
 * Desktop links that header directly; Android calls the same C++ via libarchstreamer_ds_touch.
 */
object DsTouchMapping {
    const val DS_TOUCH_NORM_MAX = 65535

    data class ViewRectF(
        val x: Float = 0f,
        val y: Float = 0f,
        val w: Float = 0f,
        val h: Float = 0f,
    ) {
        fun valid(): Boolean = w > 0f && h > 0f
    }

    /** UV fractions for Horizontal + EmphTop/EmphBot (top=left, bottom=right). */
    data class HorizontalEmphFracs(
        val leftU0: Float,
        val leftU1: Float,
        val rightU0: Float,
        val rightU1: Float,
        val leftV0: Float,
        val leftV1: Float,
        val rightV0: Float,
        val rightV1: Float,
    )

    init {
        System.loadLibrary("archstreamer_ds_touch")
    }

    fun layoutRectsLookSane(layout: DsScreenLayout?): Boolean {
        if (layout == null || !layout.hasBot || layout.windowW == 0 || layout.windowH == 0) {
            return false
        }
        if (layout.botW <= 0 || layout.botH <= 0) return false
        val botCx = layout.botX + layout.botW * 0.5f
        if (botCx < layout.windowW * 0.35f) return false
        val aspect = layout.botW.toFloat() / layout.botH.toFloat()
        return aspect in 0.45f..3.5f
    }

    fun layoutEmphasizesBottom(layout: DsScreenLayout?): Boolean =
        layoutRectsLookSane(layout) &&
            layout!!.hasTop &&
            layout.hasBot &&
            layout.botH > layout.topH

    fun clientBottomScreenHitRect(
        viewW: Float,
        viewH: Float,
        portraitStack: Boolean,
        streamAspect: Float = 16f / 9f,
        hybridRatio: Float = 3f,
        emphBottom: Boolean = false,
        screensSwapped: Boolean = false,
    ): ViewRectF? {
        val values = nativeClientBottomScreenHitRect(
            viewW,
            viewH,
            portraitStack,
            streamAspect,
            hybridRatio,
            emphBottom,
            screensSwapped,
        ) ?: return null
        if (values.size < 4) return null
        return ViewRectF(values[0], values[1], values[2], values[3])
    }

    fun resolveBottomScreenHitRect(
        viewW: Float,
        viewH: Float,
        portraitStack: Boolean,
        layout: DsScreenLayout? = null,
        streamAspect: Float = 16f / 9f,
        hybridRatio: Float = 3f,
        screensSwapped: Boolean = false,
    ): ViewRectF? {
        if (portraitStack) {
            return clientBottomScreenHitRect(
                viewW = viewW,
                viewH = viewH,
                portraitStack = true,
                streamAspect = streamAspect,
                hybridRatio = hybridRatio,
                emphBottom = layoutEmphasizesBottom(layout),
                screensSwapped = screensSwapped,
            )
        }
        val layoutOk = layoutRectsLookSane(layout)
        if (layoutOk && layout != null) {
            bottomScreenHitRect(viewW, viewH, layout)?.let { return it }
        }
        return clientBottomScreenHitRect(
            viewW = viewW,
            viewH = viewH,
            portraitStack = false,
            streamAspect = streamAspect,
            hybridRatio = hybridRatio,
            emphBottom = layoutEmphasizesBottom(layout),
            screensSwapped = false,
        )
    }

    fun horizontalEmphFracs(
        hybridRatio: Float = 3f,
        emphBottom: Boolean = false,
    ): HorizontalEmphFracs {
        val values = nativeHorizontalEmphFracs(hybridRatio, emphBottom)
        require(values != null && values.size >= 8)
        return HorizontalEmphFracs(
            leftU0 = values[0],
            leftU1 = values[1],
            rightU0 = values[2],
            rightU1 = values[3],
            leftV0 = values[4],
            leftV1 = values[5],
            rightV0 = values[6],
            rightV1 = values[7],
        )
    }

    fun bottomScreenHitRect(
        viewW: Float,
        viewH: Float,
        layout: DsScreenLayout,
    ): ViewRectF? {
        val values = nativeBottomScreenHitRect(
            viewW,
            viewH,
            layout.windowW,
            layout.windowH,
            layout.hasBot,
            layout.botX,
            layout.botY,
            layout.botW,
            layout.botH,
        ) ?: return null
        if (values.size < 4) return null
        return ViewRectF(values[0], values[1], values[2], values[3])
    }

    fun localPointToNormalized(
        localX: Float,
        localY: Float,
        localW: Float,
        localH: Float,
    ): Pair<Float, Float>? {
        val values = nativeLocalPointToNormalized(localX, localY, localW, localH) ?: return null
        if (values.size < 2) return null
        return values[0] to values[1]
    }

    fun encodeNormalizedU16(nx: Float, ny: Float): Pair<Int, Int> {
        val values = nativeEncodeNormalizedU16(nx, ny)
        require(values != null && values.size >= 2)
        return values[0] to values[1]
    }

    @JvmStatic
    private external fun nativeClientBottomScreenHitRect(
        viewW: Float,
        viewH: Float,
        portraitStack: Boolean,
        streamAspect: Float,
        hybridRatio: Float,
        emphBottom: Boolean,
        screensSwapped: Boolean,
    ): FloatArray?

    @JvmStatic
    private external fun nativeHorizontalEmphFracs(
        hybridRatio: Float,
        emphBottom: Boolean,
    ): FloatArray?

    @JvmStatic
    private external fun nativeBottomScreenHitRect(
        viewW: Float,
        viewH: Float,
        windowW: Int,
        windowH: Int,
        hasBot: Boolean,
        botX: Int,
        botY: Int,
        botW: Int,
        botH: Int,
    ): FloatArray?

    @JvmStatic
    private external fun nativeLocalPointToNormalized(
        localX: Float,
        localY: Float,
        localW: Float,
        localH: Float,
    ): FloatArray?

    @JvmStatic
    private external fun nativeEncodeNormalizedU16(nx: Float, ny: Float): IntArray?
}
