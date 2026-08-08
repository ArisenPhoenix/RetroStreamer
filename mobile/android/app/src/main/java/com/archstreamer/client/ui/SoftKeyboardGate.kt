package com.archstreamer.client.ui

import androidx.compose.runtime.Composable
import androidx.compose.ui.ExperimentalComposeUiApi
import androidx.compose.ui.platform.InterceptPlatformTextInput
import kotlinx.coroutines.awaitCancellation

/**
 * Text input wrapper kept at the activity boundary so all fields share one policy.
 *
 * When a hardware keyboard is active, do not let focused Compose text fields ask Android for
 * an input method. Android TV visibly flashes the OSK before a later hide request can run.
 * Hardware-keyboard edits are handled from key events while this gate is closed.
 */
@OptIn(ExperimentalComposeUiApi::class)
@Composable
fun WithoutSoftKeyboard(blocked: Boolean, content: @Composable () -> Unit) {
    InterceptPlatformTextInput(
        interceptor = { request, nextHandler ->
            if (blocked) awaitCancellation() else nextHandler.startInputMethod(request)
        },
        content = content,
    )
}
