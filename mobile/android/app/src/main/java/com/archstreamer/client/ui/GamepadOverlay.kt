package com.archstreamer.client.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import com.archstreamer.client.protocol.ControllerState
import kotlin.math.roundToInt

/**
 * Minimal on-screen pad that maps into ControllerState button/stick bits.
 * Left stick + d-pad + face buttons + shoulders/start — enough to play most titles.
 */
@Composable
fun GamepadOverlay(
    modifier: Modifier = Modifier,
    onState: (ControllerState) -> Unit,
) {
    var buttons by remember { mutableIntStateOf(0) }
    var leftX by remember { mutableFloatStateOf(0f) }
    var leftY by remember { mutableFloatStateOf(0f) }

    fun emit() {
        onState(
            ControllerState(
                buttons = buttons,
                leftX = (leftX * Short.MAX_VALUE).roundToInt().toShort(),
                leftY = (leftY * Short.MAX_VALUE).roundToInt().toShort(),
            ),
        )
    }

    fun setButton(mask: Int, down: Boolean) {
        buttons = if (down) buttons or mask else buttons and mask.inv()
        emit()
    }

    BoxWithConstraints(
        modifier = modifier
            .fillMaxSize()
            .padding(12.dp),
    ) {
        // Left stick
        VirtualStick(
            modifier = Modifier
                .align(Alignment.BottomStart)
                .padding(start = 8.dp, bottom = 24.dp),
            onAxis = { x, y ->
                leftX = x
                leftY = y
                emit()
            },
        )

        // D-pad
        Dpad(
            modifier = Modifier
                .align(Alignment.BottomStart)
                .padding(start = 140.dp, bottom = 36.dp),
            onButton = ::setButton,
        )

        // Face buttons
        FaceButtons(
            modifier = Modifier
                .align(Alignment.BottomEnd)
                .padding(end = 16.dp, bottom = 36.dp),
            onButton = ::setButton,
        )

        // Shoulders / start / back
        Row(
            modifier = Modifier
                .align(Alignment.TopCenter)
                .padding(top = 8.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            PadButton("L", ControllerState.BUTTON_LEFT_SHOULDER, ::setButton)
            PadButton("Select", ControllerState.BUTTON_BACK, ::setButton)
            PadButton("Start", ControllerState.BUTTON_START, ::setButton)
            PadButton("R", ControllerState.BUTTON_RIGHT_SHOULDER, ::setButton)
        }
    }
}

@Composable
private fun VirtualStick(
    modifier: Modifier = Modifier,
    onAxis: (Float, Float) -> Unit,
) {
    val size = 120.dp
    val knob = 44.dp
    val density = LocalDensity.current
    val radiusPx = with(density) { ((size - knob) / 2).toPx() }
    var knobOffset by remember { mutableStateOf(Offset.Zero) }

    Box(
        modifier = modifier
            .size(size)
            .clip(CircleShape)
            .background(Color.White.copy(alpha = 0.12f))
            .border(2.dp, Color.White.copy(alpha = 0.35f), CircleShape)
            .pointerInput(Unit) {
                detectDragGestures(
                    onDragEnd = {
                        knobOffset = Offset.Zero
                        onAxis(0f, 0f)
                    },
                    onDragCancel = {
                        knobOffset = Offset.Zero
                        onAxis(0f, 0f)
                    },
                    onDrag = { change, dragAmount ->
                        change.consume()
                        val next = knobOffset + dragAmount
                        val clamped = if (next.getDistance() > radiusPx) {
                            next * (radiusPx / next.getDistance())
                        } else {
                            next
                        }
                        knobOffset = clamped
                        onAxis(
                            (clamped.x / radiusPx).coerceIn(-1f, 1f),
                            (clamped.y / radiusPx).coerceIn(-1f, 1f),
                        )
                    },
                )
            },
        contentAlignment = Alignment.Center,
    ) {
        Box(
            modifier = Modifier
                .offset { IntOffset(knobOffset.x.roundToInt(), knobOffset.y.roundToInt()) }
                .size(knob)
                .clip(CircleShape)
                .background(Color.White.copy(alpha = 0.55f)),
        )
    }
}

@Composable
private fun Dpad(
    modifier: Modifier = Modifier,
    onButton: (Int, Boolean) -> Unit,
) {
    Column(modifier = modifier, horizontalAlignment = Alignment.CenterHorizontally) {
        PadButton("↑", ControllerState.BUTTON_DPAD_UP, onButton)
        Row {
            PadButton("←", ControllerState.BUTTON_DPAD_LEFT, onButton)
            Spacer(Modifier.width(36.dp))
            PadButton("→", ControllerState.BUTTON_DPAD_RIGHT, onButton)
        }
        PadButton("↓", ControllerState.BUTTON_DPAD_DOWN, onButton)
    }
}

@Composable
private fun FaceButtons(
    modifier: Modifier = Modifier,
    onButton: (Int, Boolean) -> Unit,
) {
    Column(modifier = modifier, horizontalAlignment = Alignment.CenterHorizontally) {
        PadButton("Y", ControllerState.BUTTON_Y, onButton)
        Row {
            PadButton("X", ControllerState.BUTTON_X, onButton)
            Spacer(Modifier.width(40.dp))
            PadButton("B", ControllerState.BUTTON_B, onButton)
        }
        PadButton("A", ControllerState.BUTTON_A, onButton)
    }
}

@Composable
private fun PadButton(
    label: String,
    mask: Int,
    onButton: (Int, Boolean) -> Unit,
) {
    var pressed by remember { mutableStateOf(false) }
    Box(
        modifier = Modifier
            .padding(4.dp)
            .size(48.dp)
            .clip(CircleShape)
            .background(
                if (pressed) MaterialTheme.colorScheme.primary.copy(alpha = 0.85f)
                else Color.White.copy(alpha = 0.18f),
            )
            .border(1.dp, Color.White.copy(alpha = 0.4f), CircleShape)
            .pointerInput(mask) {
                detectTapGestures(
                    onPress = {
                        pressed = true
                        onButton(mask, true)
                        try {
                            awaitRelease()
                        } finally {
                            pressed = false
                            onButton(mask, false)
                        }
                    },
                )
            },
        contentAlignment = Alignment.Center,
    ) {
        Text(label, color = Color.White)
    }
}
