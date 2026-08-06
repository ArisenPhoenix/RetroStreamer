package com.archstreamer.client.ui

import android.view.KeyEvent
import com.archstreamer.client.protocol.ControllerState
import com.archstreamer.client.protocol.RemotedKey

/**
 * Remoted-key bits for host VirtualKeyboard (Space FF hold on RetroArch, …).
 * Arrow keys are intentionally omitted — on mobile they map to joypad D-pad while playing.
 * Keyboard F / P are handled via EmulatorControl (absolute FF / pause toggles), not remoted.
 */
fun remotedKeyBitFromAndroidKeyCode(keyCode: Int): Int? = when (keyCode) {
    KeyEvent.KEYCODE_SPACE -> RemotedKey.SPACE
    KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER -> RemotedKey.ENTER
    KeyEvent.KEYCODE_ESCAPE -> RemotedKey.ESCAPE
    KeyEvent.KEYCODE_TAB -> RemotedKey.TAB
    KeyEvent.KEYCODE_DEL -> RemotedKey.BACKSPACE // Android DEL = Backspace
    KeyEvent.KEYCODE_F1 -> RemotedKey.F1
    KeyEvent.KEYCODE_F8 -> RemotedKey.F8
    else -> null
}

/** Keyboard arrows → ControllerState D-pad bits while playing. */
fun keyboardDpadMask(keyCode: Int): Int? = when (keyCode) {
    KeyEvent.KEYCODE_DPAD_UP -> ControllerState.BUTTON_DPAD_UP
    KeyEvent.KEYCODE_DPAD_DOWN -> ControllerState.BUTTON_DPAD_DOWN
    KeyEvent.KEYCODE_DPAD_LEFT -> ControllerState.BUTTON_DPAD_LEFT
    KeyEvent.KEYCODE_DPAD_RIGHT -> ControllerState.BUTTON_DPAD_RIGHT
    else -> null
}

fun isPlayMenuActivateKey(keyCode: Int): Boolean =
    keyCode == KeyEvent.KEYCODE_ENTER ||
        keyCode == KeyEvent.KEYCODE_NUMPAD_ENTER ||
        keyCode == KeyEvent.KEYCODE_DPAD_CENTER ||
        keyCode == KeyEvent.KEYCODE_SPACE ||
        keyCode == KeyEvent.KEYCODE_BUTTON_A

fun isPlayMenuCloseKey(keyCode: Int): Boolean =
    keyCode == KeyEvent.KEYCODE_DEL ||
        keyCode == KeyEvent.KEYCODE_ESCAPE ||
        keyCode == KeyEvent.KEYCODE_BUTTON_B

/** Play-menu actions while a session drawer is open (focus order). */
enum class PlayMenuFocusItem {
    Controls,
    GameOptions,
    Stream,
    Settings,
    Pause,
    FastForward,
    EditControls,
    SoftKeyboard,
    ResyncAv,
    Leave,
    Disconnect,
}

sealed class PlayMenuCommand {
    data object MoveUp : PlayMenuCommand()
    data object MoveDown : PlayMenuCommand()
    data object Activate : PlayMenuCommand()
    data object Close : PlayMenuCommand()
}
