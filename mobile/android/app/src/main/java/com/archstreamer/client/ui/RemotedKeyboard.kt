package com.archstreamer.client.ui

import android.view.InputDevice
import android.view.KeyEvent
import com.archstreamer.client.protocol.ControllerState
import com.archstreamer.client.protocol.RemotedKey
import com.archstreamer.client.ui.menu.NavDir

/**
 * Remoted-key bits for host VirtualKeyboard.
 * Arrow keys are intentionally omitted — on mobile they map to joypad D-pad while playing.
 * Keyboard Space / remapped pad / overlay = hold EmulatorControl FF.
 * Keyboard F / play-menu switch = latch. Pause (P) is EmulatorControl, not remoted.
 */
fun remotedKeyBitFromAndroidKeyCode(keyCode: Int): Int? = when (keyCode) {
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

/**
 * Controller Home / Guide — the PS or Xbox button — which turns the whole menu on and off.
 * Pads report it as Mode or Home; a remote's or keyboard's Menu key means the same thing.
 */
fun isMenuHomeKey(keyCode: Int): Boolean =
    keyCode == KeyEvent.KEYCODE_BUTTON_MODE ||
        keyCode == KeyEvent.KEYCODE_GUIDE ||
        keyCode == KeyEvent.KEYCODE_HOME ||
        keyCode == KeyEvent.KEYCODE_MENU

/**
 * Whether a keyboard someone can actually type on is attached.
 *
 * Android calls a lot of things keyboards: the IME registers a virtual device, and pads
 * and TV remotes register non-alphabetic keypads. A real keyboard is the non-virtual,
 * alphabetic one that is not a game controller.
 */
fun hardwareKeyboardConnected(): Boolean =
    InputDevice.getDeviceIds().any { id ->
        val device = InputDevice.getDevice(id) ?: return@any false
        isTypingKeyboard(device)
    }

/** Whether [deviceId] is something a person can type on. */
fun isTypingKeyboardDeviceId(deviceId: Int): Boolean {
    val device = InputDevice.getDevice(deviceId) ?: return false
    return isTypingKeyboard(device)
}

fun isTypingKeyboard(device: InputDevice): Boolean {
    if (device.isVirtual) return false
    if (device.keyboardType != InputDevice.KEYBOARD_TYPE_ALPHABETIC) return false
    if (PhysicalGamepad.isGameController(device)) return false
    // Android TV's virtual-remote also calls itself alphabetic; letter keys are what
    // separates a device you can type on from one that just has a D-pad.
    return device.hasKeys(KeyEvent.KEYCODE_A, KeyEvent.KEYCODE_M, KeyEvent.KEYCODE_Z).all { it }
}

/** D-pad / arrow keys → a menu direction. Both pads and TV remotes send these codes. */
fun navDirForKey(keyCode: Int): NavDir? = when (keyCode) {
    KeyEvent.KEYCODE_DPAD_UP -> NavDir.Up
    KeyEvent.KEYCODE_DPAD_DOWN -> NavDir.Down
    KeyEvent.KEYCODE_DPAD_LEFT -> NavDir.Left
    KeyEvent.KEYCODE_DPAD_RIGHT -> NavDir.Right
    else -> null
}

/**
 * Confirm / select while a menu is open.
 * South face button is [KeyEvent.KEYCODE_BUTTON_A] on Android (Xbox A / PlayStation ×).
 * TV remotes send [KEYCODE_DPAD_CENTER] or [KEYCODE_ENTER] for OK.
 */
fun isMenuActivateKey(keyCode: Int): Boolean =
    keyCode == KeyEvent.KEYCODE_ENTER ||
        keyCode == KeyEvent.KEYCODE_NUMPAD_ENTER ||
        keyCode == KeyEvent.KEYCODE_DPAD_CENTER ||
        keyCode == KeyEvent.KEYCODE_BUTTON_A

/**
 * Step back out of a section's options: East face (Xbox B). Left is first offered to the
 * focused option (slider nudge, chip move) before it counts as leaving.
 */
fun isMenuLeaveFieldsKey(keyCode: Int): Boolean =
    keyCode == KeyEvent.KEYCODE_BUTTON_B
