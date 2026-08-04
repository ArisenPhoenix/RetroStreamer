package com.archstreamer.client.ui

import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.changedToUp
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.IntOffset
import com.archstreamer.client.protocol.DsScreenLayout
import kotlin.math.roundToInt

/**
 * Transparent hit target over the DS bottom screen.
 *
 * Prefer host [DsScreenLayout] when present (follows EmphTop ↔ EmphBot). Portrait
 * stack uses the lower phone pane. Place *under* [GamepadOverlay] so overlapping
 * pad controls win hit-testing; empty pad areas pass through to this overlay.
 * Touches are normalized % within that pane; host maps to DS stylus 0–255 × 0–191.
 */
@Composable
fun DsBottomTouchOverlay(
    modifier: Modifier = Modifier,
    portraitHybridStack: Boolean,
    streamAspect: Float = 16f / 9f,
    layout: DsScreenLayout? = null,
    enabled: Boolean = true,
    onTouch: (normX: Int, normY: Int, pressed: Boolean) -> Unit,
) {
    if (!enabled) return

    BoxWithConstraints(modifier = modifier.fillMaxSize()) {
        val density = LocalDensity.current
        val viewW = constraints.maxWidth.toFloat().coerceAtLeast(1f)
        val viewH = constraints.maxHeight.toFloat().coerceAtLeast(1f)
        val hit = DsTouchMapping.resolveBottomScreenHitRect(
            viewW = viewW,
            viewH = viewH,
            portraitStack = portraitHybridStack,
            layout = layout,
            streamAspect = streamAspect,
        ) ?: return@BoxWithConstraints
        if (!hit.valid()) return@BoxWithConstraints

        val widthDp = with(density) { hit.w.toDp() }
        val heightDp = with(density) { hit.h.toDp() }
        val onTouchState = rememberUpdatedState(onTouch)

        Box(
            modifier = Modifier
                .offset { IntOffset(hit.x.roundToInt(), hit.y.roundToInt()) }
                .size(widthDp, heightDp)
                .pointerInput(hit.x, hit.y, hit.w, hit.h, layout?.botW, layout?.botH) {
                    awaitEachGesture {
                        val down = awaitFirstDown(requireUnconsumed = false)
                        down.consume()
                        emitNormalized(
                            down.position.x,
                            down.position.y,
                            size.width.toFloat(),
                            size.height.toFloat(),
                            true,
                            onTouchState.value,
                        )
                        while (true) {
                            val event = awaitPointerEvent()
                            val change = event.changes.firstOrNull() ?: break
                            if (change.changedToUp() || !change.pressed) {
                                change.consume()
                                onTouchState.value(0, 0, false)
                                break
                            }
                            change.consume()
                            emitNormalized(
                                change.position.x,
                                change.position.y,
                                size.width.toFloat(),
                                size.height.toFloat(),
                                true,
                                onTouchState.value,
                            )
                        }
                    }
                },
        )
    }
}

private fun emitNormalized(
    localX: Float,
    localY: Float,
    width: Float,
    height: Float,
    pressed: Boolean,
    onTouch: (Int, Int, Boolean) -> Unit,
) {
    val norm = DsTouchMapping.localPointToNormalized(localX, localY, width, height) ?: return
    val encoded = DsTouchMapping.encodeNormalizedU16(norm.first, norm.second)
    onTouch(encoded.first, encoded.second, pressed)
}
