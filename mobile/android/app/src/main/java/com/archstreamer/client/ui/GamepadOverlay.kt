package com.archstreamer.client.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.FastForward
import androidx.compose.material.icons.filled.Menu
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.archstreamer.client.protocol.ControllerState
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * On-screen pad driven by [items]. In [editing] mode, drag to move, remap actions, add/remove.
 */
@Composable
fun GamepadOverlay(
    modifier: Modifier = Modifier,
    items: List<OverlayItem>,
    opacity: Float = OverlayProfile.DEFAULT_OPACITY,
    editing: Boolean = false,
    onState: (ControllerState) -> Unit = {},
    onMenuClick: () -> Unit = {},
    onFastForwardHold: (Boolean) -> Unit = {},
    onItemsChange: (List<OverlayItem>) -> Unit = {},
    onDoneEditing: () -> Unit = {},
) {
    var buttons by remember { mutableIntStateOf(0) }
    var leftX by remember { mutableFloatStateOf(0f) }
    var leftY by remember { mutableFloatStateOf(0f) }
    var rightX by remember { mutableFloatStateOf(0f) }
    var rightY by remember { mutableFloatStateOf(0f) }
    var leftTrigger by remember { mutableIntStateOf(0) }
    var rightTrigger by remember { mutableIntStateOf(0) }
    var selectedId by remember { mutableStateOf<String?>(null) }
    var addMenuOpen by remember { mutableStateOf(false) }
    var actionMenuOpen by remember { mutableStateOf(false) }
    val colors = remember(opacity) { PadColors.fromOpacity(opacity) }
    val itemsLatest by rememberUpdatedState(items)
    val onItemsChangeLatest by rememberUpdatedState(onItemsChange)
    val onFastForwardHoldLatest by rememberUpdatedState(onFastForwardHold)
    val onMenuClickLatest by rememberUpdatedState(onMenuClick)

    fun emit() {
        if (editing) return
        onState(
            ControllerState(
                buttons = buttons,
                leftX = (leftX * Short.MAX_VALUE).roundToInt().toShort(),
                leftY = (leftY * Short.MAX_VALUE).roundToInt().toShort(),
                rightX = (rightX * Short.MAX_VALUE).roundToInt().toShort(),
                rightY = (rightY * Short.MAX_VALUE).roundToInt().toShort(),
                leftTrigger = leftTrigger,
                rightTrigger = rightTrigger,
            ),
        )
    }

    fun setButton(mask: Int, down: Boolean) {
        if (editing) return
        buttons = if (down) buttons or mask else buttons and mask.inv()
        emit()
    }

    fun setTrigger(left: Boolean, down: Boolean) {
        if (editing) return
        val value = if (down) 0xFFFF else 0
        if (left) leftTrigger = value else rightTrigger = value
        emit()
    }

    fun applyAction(action: OverlayAction, down: Boolean) {
        if (editing) return
        when (action) {
            OverlayAction.Default -> Unit
            OverlayAction.ButtonA -> setButton(ControllerState.BUTTON_A, down)
            OverlayAction.ButtonB -> setButton(ControllerState.BUTTON_B, down)
            OverlayAction.ButtonX -> setButton(ControllerState.BUTTON_X, down)
            OverlayAction.ButtonY -> setButton(ControllerState.BUTTON_Y, down)
            OverlayAction.ButtonL -> setButton(ControllerState.BUTTON_LEFT_SHOULDER, down)
            OverlayAction.ButtonR -> setButton(ControllerState.BUTTON_RIGHT_SHOULDER, down)
            OverlayAction.ButtonL2 -> setTrigger(left = true, down = down)
            OverlayAction.ButtonR2 -> setTrigger(left = false, down = down)
            OverlayAction.Select -> setButton(ControllerState.BUTTON_BACK, down)
            OverlayAction.Start -> setButton(ControllerState.BUTTON_START, down)
            OverlayAction.Menu -> if (down) onMenuClickLatest()
            OverlayAction.LeftStick, OverlayAction.RightStick -> Unit
            OverlayAction.FastForward -> onFastForwardHoldLatest(down)
        }
    }

    fun removeSelected() {
        val id = selectedId ?: return
        onItemsChangeLatest(itemsLatest.filterNot { it.id == id })
        selectedId = null
        actionMenuOpen = false
    }

    fun setSelectedAction(action: OverlayAction) {
        val id = selectedId ?: return
        onItemsChangeLatest(
            itemsLatest.map { item ->
                if (item.id != id) item
                else item.copy(action = action).clamped()
            },
        )
        actionMenuOpen = false
    }

    fun setSelectedScale(scale: Float) {
        val id = selectedId ?: return
        onItemsChangeLatest(
            itemsLatest.map { item ->
                if (item.id != id) item
                else item.copy(scale = scale).clamped()
            },
        )
    }

    fun addKind(kind: OverlayControlKind) {
        val current = itemsLatest
        if (current.any { it.kind == kind }) return
        val cleaned = when (kind) {
            OverlayControlKind.FaceAbxy -> current.filterNot { it.kind == OverlayControlKind.FaceAb }
            OverlayControlKind.FaceAb -> current.filterNot { it.kind == OverlayControlKind.FaceAbxy }
            else -> current
        }
        val next = OverlayPresets.spawnFor(kind)
        onItemsChangeLatest(cleaned + next)
        selectedId = next.id
        addMenuOpen = false
    }

    val presentKinds = items.map { it.kind }.toSet()
    val addable = OverlayControlKind.entries.filter { kind ->
        when (kind) {
            OverlayControlKind.FaceAbxy -> OverlayControlKind.FaceAb !in presentKinds && kind !in presentKinds
            OverlayControlKind.FaceAb -> OverlayControlKind.FaceAbxy !in presentKinds && kind !in presentKinds
            else -> kind !in presentKinds
        }
    }
    val selectedItem = items.firstOrNull { it.id == selectedId }

    BoxWithConstraints(modifier = modifier.fillMaxSize()) {
        val density = LocalDensity.current
        val widthPx = with(density) { maxWidth.toPx() }
        val heightPx = with(density) { maxHeight.toPx() }
        val widthLatest by rememberUpdatedState(widthPx)
        val heightLatest by rememberUpdatedState(heightPx)
        val shortSide = min(maxWidth.value, maxHeight.value)
        val btn = (shortSide * 0.11f).coerceIn(48f, 72f).dp
        val stick = (shortSide * 0.28f).coerceIn(110f, 160f).dp

        items.forEach { item ->
            key(item.id) {
                val selected = editing && selectedId == item.id
                val scale = item.scale
                val unitBtn = btn * scale
                val unitStick = stick * scale
                // Local position while dragging — avoids StateFlow round-trip lag / stale deltas.
                var dragCx by remember(item.id) { mutableFloatStateOf(item.cx) }
                var dragCy by remember(item.id) { mutableFloatStateOf(item.cy) }
                // Sync from props only when the saved item moves (add/reset/commit), not every frame.
                LaunchedEffect(item.cx, item.cy) {
                    dragCx = item.cx
                    dragCy = item.cy
                }
                var measured by remember(item.id) { mutableStateOf(IntSize.Zero) }

                Box(
                    modifier = Modifier
                        .align(Alignment.TopStart)
                        .onSizeChanged { measured = it }
                        .offset {
                            IntOffset(
                                (dragCx * widthPx - measured.width / 2f).roundToInt(),
                                (dragCy * heightPx - measured.height / 2f).roundToInt(),
                            )
                        }
                        .then(
                            if (editing) {
                                Modifier
                                    // Keep border width constant so selection doesn't re-measure and jump.
                                    .border(
                                        width = 2.dp,
                                        color = if (selected) Color(0xFF4ADE80) else Color(0x66FFFFFF),
                                        shape = RoundedCornerShape(8.dp),
                                    )
                                    .padding(4.dp)
                                    .pointerInput(item.id) {
                                        // Tap selects; drag past touch-slop moves.
                                        awaitEachGesture {
                                            val down = awaitFirstDown(requireUnconsumed = false)
                                            selectedId = item.id
                                            val slop = viewConfiguration.touchSlop
                                            var movedX = 0f
                                            var movedY = 0f
                                            var dragging = false
                                            try {
                                                while (true) {
                                                    val event = awaitPointerEvent()
                                                    val change = event.changes.firstOrNull { it.id == down.id }
                                                        ?: break
                                                    if (!change.pressed) break
                                                    val dx = change.position.x - change.previousPosition.x
                                                    val dy = change.position.y - change.previousPosition.y
                                                    movedX += dx
                                                    movedY += dy
                                                    if (!dragging &&
                                                        (movedX * movedX + movedY * movedY) > slop * slop
                                                    ) {
                                                        dragging = true
                                                    }
                                                    if (dragging) {
                                                        change.consume()
                                                        val w = widthLatest
                                                        val h = heightLatest
                                                        if (w > 0f && h > 0f) {
                                                            dragCx = (dragCx + dx / w).coerceIn(0.02f, 0.98f)
                                                            dragCy = (dragCy + dy / h).coerceIn(0.02f, 0.98f)
                                                        }
                                                    }
                                                }
                                            } finally {
                                                if (dragging) {
                                                    val id = item.id
                                                    val cx = dragCx
                                                    val cy = dragCy
                                                    onItemsChangeLatest(
                                                        itemsLatest.map { existing ->
                                                            if (existing.id != id) existing
                                                            else existing.copy(cx = cx, cy = cy).clamped()
                                                        },
                                                    )
                                                }
                                            }
                                        }
                                    }
                            } else {
                                Modifier
                            },
                        ),
                ) {
                    val label = item.displayLabel()
                    val useFfIcon = item.resolvedAction() == OverlayAction.FastForward
                    when (item.kind) {
                        OverlayControlKind.Menu -> {
                            MenuPadButton(
                                colors = colors,
                                size = unitBtn * 0.9f,
                                enabled = !editing,
                                onClick = {
                                    applyAction(item.resolvedAction(), true)
                                    applyAction(item.resolvedAction(), false)
                                },
                            )
                        }
                        OverlayControlKind.ShoulderL,
                        OverlayControlKind.ShoulderR,
                        OverlayControlKind.ShoulderL2,
                        OverlayControlKind.ShoulderR2,
                        OverlayControlKind.FastForward,
                        -> {
                            ShoulderButton(
                                label = label,
                                useFastForwardIcon = useFfIcon,
                                width = unitBtn * 1.55f,
                                height = unitBtn * 0.72f,
                                colors = colors,
                                enabled = !editing,
                                onPress = { down -> applyAction(item.resolvedAction(), down) },
                            )
                        }
                        OverlayControlKind.Select,
                        OverlayControlKind.Start,
                        -> {
                            PadButton(
                                label = label,
                                useFastForwardIcon = useFfIcon,
                                mask = 0,
                                size = unitBtn * 0.85f,
                                colors = colors,
                                onButton = { _, down -> applyAction(item.resolvedAction(), down) },
                                enabled = !editing,
                            )
                        }
                        OverlayControlKind.LeftStick -> {
                            VirtualStick(
                                size = unitStick,
                                colors = colors,
                                enabled = !editing,
                                onAxis = { x, y ->
                                    when (item.resolvedAction()) {
                                        OverlayAction.RightStick -> {
                                            rightX = x
                                            rightY = y
                                        }
                                        else -> {
                                            leftX = x
                                            leftY = y
                                        }
                                    }
                                    emit()
                                },
                            )
                        }
                        OverlayControlKind.RightStick -> {
                            VirtualStick(
                                size = unitStick,
                                colors = colors,
                                enabled = !editing,
                                onAxis = { x, y ->
                                    when (item.resolvedAction()) {
                                        OverlayAction.LeftStick -> {
                                            leftX = x
                                            leftY = y
                                        }
                                        else -> {
                                            rightX = x
                                            rightY = y
                                        }
                                    }
                                    emit()
                                },
                            )
                        }
                        OverlayControlKind.Dpad -> {
                            Dpad(buttonSize = unitBtn, colors = colors, enabled = !editing, onButton = ::setButton)
                        }
                        OverlayControlKind.FaceAbxy -> {
                            FaceButtons(buttonSize = unitBtn, colors = colors, enabled = !editing, onButton = ::setButton)
                        }
                        OverlayControlKind.FaceAb -> {
                            FaceButtonsAbOnly(buttonSize = unitBtn, colors = colors, enabled = !editing, onButton = ::setButton)
                        }
                    }
                }
            }
        }

        if (editing) {
            // Compact vertical toolbox — draggable so it never covers the pad forever.
            var panelOffset by remember { mutableStateOf(Offset.Zero) }
            Surface(
                modifier = Modifier
                    .align(Alignment.TopEnd)
                    .padding(10.dp)
                    .offset {
                        IntOffset(panelOffset.x.roundToInt(), panelOffset.y.roundToInt())
                    }
                    .widthIn(min = 148.dp, max = 180.dp),
                shape = RoundedCornerShape(14.dp),
                color = MaterialTheme.colorScheme.surface.copy(alpha = 0.96f),
                tonalElevation = 6.dp,
            ) {
                Column(
                    Modifier.padding(horizontal = 10.dp, vertical = 8.dp),
                    verticalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .height(28.dp)
                            .clip(RoundedCornerShape(8.dp))
                            .background(MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.7f))
                            .pointerInput(Unit) {
                                detectDragGestures { change, dragAmount ->
                                    change.consume()
                                    panelOffset += dragAmount
                                }
                            },
                        contentAlignment = Alignment.Center,
                    ) {
                        Text(
                            "⋮⋮ drag",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    Text(
                        "Edit pad",
                        style = MaterialTheme.typography.labelLarge,
                    )
                    Box(modifier = Modifier.fillMaxWidth()) {
                        FilledTonalButton(
                            onClick = { addMenuOpen = true },
                            enabled = addable.isNotEmpty(),
                            modifier = Modifier.fillMaxWidth(),
                        ) {
                            Icon(Icons.Filled.Add, contentDescription = null, modifier = Modifier.size(18.dp))
                            Spacer(Modifier.width(4.dp))
                            Text("Add")
                        }
                        DropdownMenu(
                            expanded = addMenuOpen,
                            onDismissRequest = { addMenuOpen = false },
                        ) {
                            addable.forEach { kind ->
                                DropdownMenuItem(
                                    text = { Text(kind.title) },
                                    onClick = { addKind(kind) },
                                )
                            }
                        }
                    }
                    Box(modifier = Modifier.fillMaxWidth()) {
                        FilledTonalButton(
                            onClick = { actionMenuOpen = true },
                            enabled = selectedItem?.kind?.remappable == true,
                            modifier = Modifier.fillMaxWidth(),
                        ) {
                            Text("Action")
                        }
                        DropdownMenu(
                            expanded = actionMenuOpen,
                            onDismissRequest = { actionMenuOpen = false },
                        ) {
                            DropdownMenuItem(
                                text = {
                                    Text("Default (${selectedItem?.kind?.defaultAction?.title ?: "—"})")
                                },
                                onClick = { setSelectedAction(OverlayAction.Default) },
                            )
                            OverlayAction.remappable.forEach { action ->
                                DropdownMenuItem(
                                    text = { Text(action.title) },
                                    onClick = { setSelectedAction(action) },
                                )
                            }
                        }
                    }
                    FilledTonalButton(
                        onClick = ::removeSelected,
                        enabled = selectedId != null,
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        Icon(Icons.Filled.Delete, contentDescription = null, modifier = Modifier.size(18.dp))
                        Spacer(Modifier.width(4.dp))
                        Text("Remove")
                    }
                    Button(
                        onClick = onDoneEditing,
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        Icon(Icons.Filled.Check, contentDescription = null, modifier = Modifier.size(18.dp))
                        Spacer(Modifier.width(4.dp))
                        Text("Save")
                    }
                    selectedItem?.let { sel ->
                        val actionLabel = when {
                            !sel.kind.remappable -> "fixed cluster"
                            sel.action == OverlayAction.Default ->
                                "→ ${sel.kind.defaultAction.title}"
                            else -> sel.resolvedAction().title
                        }
                        Text(
                            "${sel.kind.title}\n$actionLabel",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                        Text(
                            "Size ${(sel.scale * 100f).roundToInt()}%",
                            style = MaterialTheme.typography.labelSmall,
                        )
                        Slider(
                            value = sel.scale,
                            onValueChange = ::setSelectedScale,
                            valueRange = 0.55f..1.75f,
                            modifier = Modifier.fillMaxWidth(),
                        )
                    }
                }
            }
        }

        // Fallback menu if the Menu item was removed.
        if (!editing && items.none { it.kind == OverlayControlKind.Menu }) {
            IconButton(
                onClick = onMenuClick,
                modifier = Modifier
                    .align(Alignment.TopStart)
                    .padding(8.dp),
            ) {
                Icon(Icons.Filled.Menu, contentDescription = "Menu", tint = Color.White)
            }
        }
    }
}

private data class PadColors(
    val fill: Color,
    val fillPressed: Color,
    val border: Color,
    val label: Color,
    val tray: Color,
    val knob: Color,
    val knobBorder: Color,
) {
    companion object {
        private fun argb(alpha: Int, rgb: Int): Color {
            val a = alpha.coerceIn(0, 255)
            return Color(((a.toLong() and 0xFF) shl 24) or (rgb.toLong() and 0xFFFFFF))
        }

        fun fromOpacity(opacity: Float): PadColors {
            val a = opacityToAlpha(opacity)
            val pressedA = ((opacity.coerceIn(0f, 1f) * 0.95f + 0.05f) * 255f).roundToInt().coerceIn(0, 255)
            val trayA = ((opacity.coerceIn(0f, 1f) * 0.80f) * 255f).roundToInt().coerceIn(0, 255)
            return PadColors(
                fill = argb(a, 0x121816),
                fillPressed = argb(pressedA, 0x2E7D4F),
                border = argb(a, 0xFFFFFF),
                label = argb(a.coerceAtLeast(180), 0xF5F7F6),
                tray = argb(trayA, 0x0A0E0C),
                knob = argb(a, 0xFFFFFF),
                knobBorder = argb(a, 0x1A2220),
            )
        }
    }
}

@Composable
private fun MenuPadButton(
    colors: PadColors,
    size: Dp,
    enabled: Boolean,
    onClick: () -> Unit,
) {
    Box(
        modifier = Modifier
            .size(size)
            .clip(CircleShape)
            .background(colors.fill)
            .border(2.dp, colors.border, CircleShape)
            .then(
                if (enabled) {
                    Modifier.pointerInput(Unit) {
                        awaitEachGesture {
                            awaitFirstDown(requireUnconsumed = false)
                            do {
                                val event = awaitPointerEvent()
                                val change = event.changes.firstOrNull()
                                if (change == null || !change.pressed) break
                                change.consume()
                            } while (true)
                            onClick()
                        }
                    }
                } else {
                    Modifier
                },
            ),
        contentAlignment = Alignment.Center,
    ) {
        Icon(
            Icons.Filled.Menu,
            contentDescription = "Menu",
            tint = colors.label,
            modifier = Modifier.size(size * 0.45f),
        )
    }
}

@Composable
private fun ShoulderButton(
    label: String,
    width: Dp,
    height: Dp,
    colors: PadColors,
    enabled: Boolean = true,
    useFastForwardIcon: Boolean = false,
    onPress: (Boolean) -> Unit,
) {
    var pressed by remember { mutableStateOf(false) }
    val shape = RoundedCornerShape(10.dp)
    Box(
        modifier = Modifier
            .width(width)
            .height(height)
            .clip(shape)
            .background(if (pressed) colors.fillPressed else colors.fill)
            .border(2.dp, colors.border, shape)
            .then(
                if (enabled) {
                    Modifier.pointerInput(width, height) {
                        awaitEachGesture {
                            val down = awaitFirstDown(requireUnconsumed = false)
                            pressed = true
                            onPress(true)
                            try {
                                do {
                                    val event = awaitPointerEvent()
                                    val change = event.changes.firstOrNull { it.id == down.id }
                                    if (change == null || !change.pressed) break
                                    change.consume()
                                } while (true)
                            } finally {
                                pressed = false
                                onPress(false)
                            }
                        }
                    }
                } else {
                    Modifier
                },
            ),
        contentAlignment = Alignment.Center,
    ) {
        if (useFastForwardIcon) {
            Icon(
                Icons.Filled.FastForward,
                contentDescription = "Fast-forward",
                tint = colors.label,
                modifier = Modifier.size(height * 0.58f),
            )
        } else {
            Text(
                text = label,
                color = colors.label,
                fontWeight = FontWeight.Bold,
                fontSize = (height.value * 0.36f).coerceIn(10f, 16f).sp,
            )
        }
    }
}

@Composable
private fun VirtualStick(
    size: Dp,
    colors: PadColors,
    enabled: Boolean = true,
    onAxis: (Float, Float) -> Unit,
) {
    val knob = size * 0.38f
    val density = LocalDensity.current
    val radiusPx = with(density) { ((size - knob) / 2).toPx() }
    var knobOffset by remember { mutableStateOf(Offset.Zero) }

    Box(
        modifier = Modifier
            .size(size)
            .clip(CircleShape)
            .background(colors.fill)
            .border(2.dp, colors.border, CircleShape)
            .then(
                if (enabled) {
                    Modifier.pointerInput(size) {
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
                    }
                } else {
                    Modifier
                },
            ),
        contentAlignment = Alignment.Center,
    ) {
        Box(
            modifier = Modifier
                .offset { IntOffset(knobOffset.x.roundToInt(), knobOffset.y.roundToInt()) }
                .size(knob)
                .clip(CircleShape)
                .background(colors.knob)
                .border(1.dp, colors.knobBorder, CircleShape),
        )
    }
}

@Composable
private fun Dpad(
    buttonSize: Dp,
    colors: PadColors,
    enabled: Boolean = true,
    onButton: (Int, Boolean) -> Unit,
) {
    val gap = buttonSize * 0.15f
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        PadButton("↑", ControllerState.BUTTON_DPAD_UP, buttonSize, colors, onButton, enabled)
        Row(verticalAlignment = Alignment.CenterVertically) {
            PadButton("←", ControllerState.BUTTON_DPAD_LEFT, buttonSize, colors, onButton, enabled)
            Spacer(Modifier.width(gap))
            Spacer(Modifier.size(buttonSize))
            Spacer(Modifier.width(gap))
            PadButton("→", ControllerState.BUTTON_DPAD_RIGHT, buttonSize, colors, onButton, enabled)
        }
        PadButton("↓", ControllerState.BUTTON_DPAD_DOWN, buttonSize, colors, onButton, enabled)
    }
}

@Composable
private fun FaceButtons(
    buttonSize: Dp,
    colors: PadColors,
    enabled: Boolean = true,
    onButton: (Int, Boolean) -> Unit,
) {
    val gap = buttonSize * 0.2f
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        PadButton("Y", ControllerState.BUTTON_Y, buttonSize, colors, onButton, enabled)
        Spacer(Modifier.height(gap))
        Row(verticalAlignment = Alignment.CenterVertically) {
            PadButton("X", ControllerState.BUTTON_X, buttonSize, colors, onButton, enabled)
            Spacer(Modifier.width(buttonSize + gap))
            PadButton("B", ControllerState.BUTTON_B, buttonSize, colors, onButton, enabled)
        }
        Spacer(Modifier.height(gap))
        PadButton("A", ControllerState.BUTTON_A, buttonSize, colors, onButton, enabled)
    }
}

@Composable
private fun FaceButtonsAbOnly(
    buttonSize: Dp,
    colors: PadColors,
    enabled: Boolean = true,
    onButton: (Int, Boolean) -> Unit,
) {
    val gap = buttonSize * 0.35f
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        PadButton("B", ControllerState.BUTTON_B, buttonSize, colors, onButton, enabled)
        Spacer(Modifier.height(gap))
        PadButton("A", ControllerState.BUTTON_A, buttonSize, colors, onButton, enabled)
    }
}

@Composable
private fun PadButton(
    label: String,
    mask: Int,
    size: Dp,
    colors: PadColors,
    onButton: (Int, Boolean) -> Unit,
    enabled: Boolean = true,
    useFastForwardIcon: Boolean = false,
) {
    var pressed by remember { mutableStateOf(false) }
    Box(
        modifier = Modifier
            .size(size)
            .widthIn(min = size)
            .clip(CircleShape)
            .background(if (pressed) colors.fillPressed else colors.fill)
            .border(2.dp, colors.border, CircleShape)
            .then(
                if (enabled) {
                    Modifier.pointerInput(mask, size) {
                        awaitEachGesture {
                            val down = awaitFirstDown(requireUnconsumed = false)
                            pressed = true
                            onButton(mask, true)
                            try {
                                do {
                                    val event = awaitPointerEvent()
                                    val change = event.changes.firstOrNull { it.id == down.id }
                                    if (change == null || !change.pressed) break
                                    change.consume()
                                } while (true)
                            } finally {
                                pressed = false
                                onButton(mask, false)
                            }
                        }
                    }
                } else {
                    Modifier
                },
            ),
        contentAlignment = Alignment.Center,
    ) {
        if (useFastForwardIcon) {
            Icon(
                Icons.Filled.FastForward,
                contentDescription = "Fast-forward",
                tint = colors.label,
                modifier = Modifier.size(size * 0.45f),
            )
        } else {
            Text(
                text = label,
                color = colors.label,
                fontWeight = FontWeight.Bold,
                fontSize = (size.value * 0.28f).coerceIn(12f, 18f).sp,
            )
        }
    }
}
