package com.archstreamer.client.ui

import androidx.compose.runtime.Composable
import androidx.compose.ui.ExperimentalComposeUiApi
import androidx.compose.ui.platform.InterceptPlatformTextInput
import kotlinx.coroutines.awaitCancellation

/**
 * Keeps the on-screen keyboard out of the way of a real one.
 *
 * `KeyboardOptions.showKeyboardOnFocus` cannot do this: the value / onValueChange text
 * fields this app uses document that they ignore it, so the IME rises whenever a field
 * takes focus and hiding it afterwards is what you see as a flash. Refusing to start the
 * input method means it never appears in the first place.
 *
 * Hardware keys reach the focused field through key input rather than through the input
 * method, so typing on the attached keyboard is unaffected.
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
