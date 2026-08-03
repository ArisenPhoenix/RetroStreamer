package com.archstreamer.client.ui

import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.changedToUp
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.IntOffset
import kotlin.math.roundToInt

private const val DsTouchWidth = 256
private const val DsTouchHeight = 192
/** Matches melonDS / ArchStreamer hybrid_ratio used for EmphTop width split approx. */
private const val HybridRatio = 3f

/**
 * Transparent hit target over the DS bottom screen. Place *under* [GamepadOverlay]
 * in the Compose tree so overlapping pad controls win hit-testing (draw order = z).
 *
 * Portrait Hybrid stack: lower 4:3 pane.
 * Landscape letterbox: right (bottom-screen) column of Horizontal+EmphTop video.
 */
@Composable
fun DsBottomTouchOverlay(
    modifier: Modifier = Modifier,
    portraitHybridStack: Boolean,
    streamAspect: Float = 16f / 9f,
    enabled: Boolean = true,
    onTouch: (x: Int, y: Int, pressed: Boolean) -> Unit,
) {
    if (!enabled) return

    BoxWithConstraints(modifier = modifier.fillMaxSize()) {
        val density = LocalDensity.current
        val viewW = constraints.maxWidth.toFloat().coerceAtLeast(1f)
        val viewH = constraints.maxHeight.toFloat().coerceAtLeast(1f)
        val ratio = streamAspect.coerceAtLeast(0.1f)
        val rect = if (portraitHybridStack && ratio > 1.15f) {
            portraitBottomPane(viewW, viewH)
        } else {
            landscapeBottomPane(viewW, viewH, ratio)
        }

        val widthDp = with(density) { rect.width.toDp() }
        val heightDp = with(density) { rect.height.toDp() }

        Box(
            modifier = Modifier
                .offset { IntOffset(rect.left.roundToInt(), rect.top.roundToInt()) }
                .size(widthDp, heightDp)
                .pointerInput(rect.width, rect.height, onTouch) {
                    awaitEachGesture {
                        val down = awaitFirstDown(requireUnconsumed = false)
                        onTouch(
                            mapX(down.position.x, size.width.toFloat()),
                            mapY(down.position.y, size.height.toFloat()),
                            true,
                        )
                        while (true) {
                            val event = awaitPointerEvent()
                            val change = event.changes.firstOrNull() ?: break
                            if (change.changedToUp() || !change.pressed) {
                                onTouch(0, 0, false)
                                break
                            }
                            change.consume()
                            onTouch(
                                mapX(change.position.x, size.width.toFloat()),
                                mapY(change.position.y, size.height.toFloat()),
                                true,
                            )
                        }
                    }
                },
        )
    }
}

private data class TouchRect(val left: Float, val top: Float, val width: Float, val height: Float)

private fun portraitBottomPane(viewW: Float, viewH: Float): TouchRect {
    val paneH = viewW * 0.75f
    val totalH = paneH * 2f
    val originY = ((viewH - totalH) * 0.5f).coerceAtLeast(0f)
    val usedPaneH = if (totalH > viewH) viewH * 0.5f else paneH
    return TouchRect(
        left = 0f,
        top = originY + usedPaneH,
        width = viewW,
        height = usedPaneH,
    )
}

private fun landscapeBottomPane(viewW: Float, viewH: Float, streamAspect: Float): TouchRect {
    val boxW: Float
    val boxH: Float
    if (viewW / viewH > streamAspect) {
        boxH = viewH
        boxW = viewH * streamAspect
    } else {
        boxW = viewW
        boxH = viewW / streamAspect
    }
    val originX = (viewW - boxW) * 0.5f
    val originY = (viewH - boxH) * 0.5f

    // Horizontal + EmphTop ≈ large top (left) + small bottom (right, full height).
    val r = HybridRatio.coerceIn(2f, 4f)
    val nativeW = 256f
    val primaryW = nativeW * r
    val bufferW = primaryW + nativeW + 2f * r
    val split = (primaryW / bufferW).coerceIn(0.5f, 0.9f)
    val smallRight = (primaryW + nativeW) / bufferW

    return TouchRect(
        left = originX + boxW * split,
        top = originY,
        width = boxW * (smallRight - split),
        height = boxH,
    )
}

private fun mapX(localX: Float, width: Float): Int {
    if (width <= 0f) return 0
    return ((localX / width) * (DsTouchWidth - 1)).roundToInt().coerceIn(0, DsTouchWidth - 1)
}

private fun mapY(localY: Float, height: Float): Int {
    if (height <= 0f) return 0
    return ((localY / height) * (DsTouchHeight - 1)).roundToInt().coerceIn(0, DsTouchHeight - 1)
}
