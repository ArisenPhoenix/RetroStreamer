package com.archstreamer.client.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.focusable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.composed
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.unit.dp

/**
 * Focus chrome for dialogs, which use Compose focus rather than the section/option model
 * in [com.archstreamer.client.ui.menu]. [FocusState.hasFocus] covers cases where a child
 * owns the focus target.
 */
fun Modifier.controllerFocusRing(
    shape: RoundedCornerShape = RoundedCornerShape(10.dp),
): Modifier = composed {
    var highlighted by remember { mutableStateOf(false) }
    val borderColor = MaterialTheme.colorScheme.primary
    val fill = MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.85f)
    this
        .onFocusChanged { highlighted = it.isFocused || it.hasFocus }
        .then(
            if (highlighted) {
                Modifier
                    .background(fill, shape)
                    .border(3.dp, SolidColor(borderColor), shape)
            } else {
                Modifier
            },
        )
}

/** Non-focusable container that should join the D-pad order. */
fun Modifier.controllerFocusable(enabled: Boolean = true): Modifier =
    if (enabled) this.focusable().controllerFocusRing() else this
