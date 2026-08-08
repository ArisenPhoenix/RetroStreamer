package com.archstreamer.client.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusProperties
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.unit.dp
import com.archstreamer.client.protocol.SoftKeyboardRequest
import kotlinx.coroutines.yield

/**
 * Host SoftKeyboardRequest UI — submit one string (or cancel).
 * Matches desktop PadOnScreenKeyboard semantics over TCP SoftKeyboardResponse.
 * D-pad / B / A work for controller navigation between field and buttons.
 */
@Composable
fun SoftKeyboardDialog(
    request: SoftKeyboardRequest,
    onSubmit: (text: String) -> Unit,
    onCancel: () -> Unit,
) {
    val maxLen = request.maxLength.coerceIn(1, 64)
    var text by remember(request.requestId) {
        mutableStateOf(sanitizeSoftKeyboardText(request.initialText, maxLen))
    }
    val fieldFocus = remember { FocusRequester() }
    val okFocus = remember { FocusRequester() }
    val cancelFocus = remember { FocusRequester() }

    LaunchedEffect(request.requestId) {
        yield()
        runCatching { fieldFocus.requestFocus() }
    }

    fun trySubmit() {
        val trimmed = text.trim()
        if (trimmed.isNotEmpty()) onSubmit(trimmed)
    }

    AlertDialog(
        onDismissRequest = onCancel,
        modifier = Modifier.onPreviewKeyEvent { event ->
            if (event.type != KeyEventType.KeyDown) return@onPreviewKeyEvent false
            when (event.key) {
                Key.Back, Key.Escape, Key.ButtonB -> {
                    onCancel()
                    true
                }
                // Desktop's pad keyboard accepts on R2 or Start; match it so the same
                // muscle memory submits here.
                Key.ButtonR2, Key.ButtonStart -> {
                    trySubmit()
                    true
                }
                else -> false
            }
        },
        title = {
            Text(
                request.prompt.ifBlank { "Software Keyboard" },
                style = MaterialTheme.typography.titleMedium,
            )
        },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(
                    "Host will type this into the emulator dialog (A–Z, 0–9, space, _ , -).",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                OutlinedTextField(
                    value = text,
                    onValueChange = { raw ->
                        text = sanitizeSoftKeyboardText(raw, maxLen)
                    },
                    singleLine = true,
                    modifier = Modifier
                        .fillMaxWidth()
                        .focusRequester(fieldFocus)
                        .focusProperties {
                            next = okFocus
                            down = okFocus
                        }
                        .controllerFocusRing(),
                    keyboardOptions = KeyboardOptions(
                        capitalization = KeyboardCapitalization.Words,
                        imeAction = ImeAction.Done,
                    ),
                    keyboardActions = KeyboardActions(
                        onDone = { trySubmit() },
                    ),
                    supportingText = {
                        Text("${text.length} / $maxLen")
                    },
                )
            }
        },
        confirmButton = {
            TextButton(
                onClick = { trySubmit() },
                enabled = text.trim().isNotEmpty(),
                modifier = Modifier
                    .focusRequester(okFocus)
                    .focusProperties {
                        previous = fieldFocus
                        next = cancelFocus
                        up = fieldFocus
                        left = cancelFocus
                        right = cancelFocus
                    }
                    .controllerFocusable(enabled = text.trim().isNotEmpty()),
            ) {
                Text("OK")
            }
        },
        dismissButton = {
            Row(modifier = Modifier.padding(end = 4.dp)) {
                TextButton(
                    onClick = onCancel,
                    modifier = Modifier
                        .focusRequester(cancelFocus)
                        .focusProperties {
                            previous = okFocus
                            next = fieldFocus
                            up = fieldFocus
                            left = okFocus
                            right = okFocus
                        }
                        .controllerFocusable(),
                ) {
                    Text("Cancel")
                }
            }
        },
    )
}

/** Host XTest inject accepts A–Z a–z 0–9 space _ - and truncates to maxLen. */
fun sanitizeSoftKeyboardText(raw: String, maxLen: Int): String {
    val filtered = buildString(raw.length) {
        for (ch in raw) {
            when {
                ch in 'A'..'Z' || ch in 'a'..'z' -> append(ch)
                ch in '0'..'9' -> append(ch)
                ch == ' ' || ch == '_' || ch == '-' -> append(ch)
            }
        }
    }
    return if (filtered.length <= maxLen) filtered else filtered.take(maxLen)
}
