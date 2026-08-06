package com.archstreamer.client.ui

import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import com.archstreamer.client.protocol.ControllerState
import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * USB / Bluetooth gamepads exposed through Android's InputDevice stack.
 * Face-button swaps stay in [ClientViewModel]; overlay Action remaps are applied here.
 */
object PhysicalGamepad {
    data class ConnectedPad(
        val deviceId: Int,
        val name: String,
        val descriptor: String,
    )

    fun connectedPads(): List<ConnectedPad> =
        InputDevice.getDeviceIds().asList().mapNotNull { id ->
            val device = InputDevice.getDevice(id) ?: return@mapNotNull null
            if (!isGameController(device)) return@mapNotNull null
            ConnectedPad(
                deviceId = id,
                name = device.name.ifBlank { "Gamepad $id" },
                descriptor = device.descriptor.ifBlank { "android-pad-$id" },
            )
        }

    fun isGameController(device: InputDevice): Boolean {
        if (device.isVirtual) return false
        val sources = device.sources
        val gamepad = sources and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD
        val joystick = sources and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK
        // Real gamepads always qualify. Joystick-only is also used by many BT keyboards, so
        // require at least one pad button before treating those as controllers.
        if (gamepad) return true
        if (!joystick) return false
        val padKeys = device.hasKeys(
            KeyEvent.KEYCODE_BUTTON_A,
            KeyEvent.KEYCODE_BUTTON_B,
            KeyEvent.KEYCODE_BUTTON_X,
            KeyEvent.KEYCODE_BUTTON_Y,
            KeyEvent.KEYCODE_BUTTON_L1,
            KeyEvent.KEYCODE_BUTTON_R1,
            KeyEvent.KEYCODE_BUTTON_START,
            KeyEvent.KEYCODE_BUTTON_SELECT,
        )
        return padKeys.any { it }
    }

    fun isGameControllerDeviceId(deviceId: Int): Boolean {
        if (deviceId < 0) return false
        val device = InputDevice.getDevice(deviceId) ?: return false
        return isGameController(device)
    }

    /** Physical key → overlay control kind (for Action remaps). Face cluster is not remappable. */
    fun controlKindForKey(keyCode: Int): OverlayControlKind? = when (keyCode) {
        KeyEvent.KEYCODE_BUTTON_SELECT -> OverlayControlKind.Select
        KeyEvent.KEYCODE_BUTTON_START -> OverlayControlKind.Start
        KeyEvent.KEYCODE_BUTTON_L1 -> OverlayControlKind.ShoulderL
        KeyEvent.KEYCODE_BUTTON_R1 -> OverlayControlKind.ShoulderR
        KeyEvent.KEYCODE_BUTTON_L2 -> OverlayControlKind.ShoulderL2
        KeyEvent.KEYCODE_BUTTON_R2 -> OverlayControlKind.ShoulderR2
        KeyEvent.KEYCODE_BUTTON_THUMBL -> OverlayControlKind.LeftStick
        KeyEvent.KEYCODE_BUTTON_THUMBR -> OverlayControlKind.RightStick
        else -> null
    }
}

/**
 * Accumulates KeyEvent / MotionEvent from a physical pad into [ControllerState].
 * Home / Mode opens the play menu. [actionFor] supplies overlay Action remaps
 * (e.g. Select → Fast-forward) so custom layouts apply to the physical pad.
 */
class PhysicalGamepadTracker(
    private val actionFor: (OverlayControlKind) -> OverlayAction,
    private val onState: (ControllerState) -> Unit,
    private val onMenuClick: () -> Unit,
    private val onFastForward: (Boolean) -> Unit,
    private val onScreenSwap: () -> Unit,
) {
    private var buttons: Int = 0
    private var leftX: Short = 0
    private var leftY: Short = 0
    private var rightX: Short = 0
    private var rightY: Short = 0
    private var leftTrigger: Int = 0
    private var rightTrigger: Int = 0
    private var leftTriggerButton = false
    private var rightTriggerButton = false
    private var leftTriggerAxis = 0f
    private var rightTriggerAxis = 0f
    private var ffFromL2 = false
    private var ffFromR2 = false
    private var ffFromDigital = false
    private var swapFromL2 = false
    private var swapFromR2 = false

    fun reset() {
        clearFastForward()
        buttons = 0
        leftX = 0
        leftY = 0
        rightX = 0
        rightY = 0
        leftTrigger = 0
        rightTrigger = 0
        leftTriggerButton = false
        rightTriggerButton = false
        leftTriggerAxis = 0f
        rightTriggerAxis = 0f
        emit()
    }

    /** @return true if the event was consumed as gamepad input. */
    fun handleKeyEvent(event: KeyEvent): Boolean {
        if (!PhysicalGamepad.isGameControllerDeviceId(event.deviceId)) return false
        val keyCode = event.keyCode
        if (isMenuKey(keyCode)) {
            if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) {
                onMenuClick()
            }
            return true
        }
        if (event.action != KeyEvent.ACTION_DOWN && event.action != KeyEvent.ACTION_UP) {
            return true
        }
        val down = event.action == KeyEvent.ACTION_DOWN

        val kind = PhysicalGamepad.controlKindForKey(keyCode)
        if (kind != null) {
            when (kind) {
                OverlayControlKind.ShoulderL2 -> leftTriggerButton = down
                OverlayControlKind.ShoulderR2 -> rightTriggerButton = down
                else -> Unit
            }
            dispatchAction(actionFor(kind), down, fromTriggerKind = kind)
            refreshTriggers()
            emit()
            return true
        }

        val mask = buttonMask(keyCode)
        if (mask == 0) return false
        setButton(mask, down)
        emit()
        return true
    }

    /** @return true if the event was consumed as gamepad input. */
    fun handleMotionEvent(event: MotionEvent): Boolean {
        if (!PhysicalGamepad.isGameControllerDeviceId(event.deviceId)) return false
        val sources = event.source
        val fromStick =
            sources and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK ||
                sources and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD
        if (!fromStick) return false

        leftX = axisToShort(event.getAxisValue(MotionEvent.AXIS_X))
        leftY = axisToShort(event.getAxisValue(MotionEvent.AXIS_Y))
        rightX = axisToShort(firstAxis(event, MotionEvent.AXIS_Z, MotionEvent.AXIS_RX))
        rightY = axisToShort(firstAxis(event, MotionEvent.AXIS_RZ, MotionEvent.AXIS_RY))

        leftTriggerAxis = triggerAxis(event, MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_BRAKE)
        rightTriggerAxis = triggerAxis(event, MotionEvent.AXIS_RTRIGGER, MotionEvent.AXIS_GAS)
        refreshTriggers()

        val hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X)
        val hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
        setButton(ControllerState.BUTTON_DPAD_LEFT, hatX < -0.5f)
        setButton(ControllerState.BUTTON_DPAD_RIGHT, hatX > 0.5f)
        setButton(ControllerState.BUTTON_DPAD_UP, hatY < -0.5f)
        setButton(ControllerState.BUTTON_DPAD_DOWN, hatY > 0.5f)

        emit()
        return true
    }

    private fun dispatchAction(
        action: OverlayAction,
        down: Boolean,
        fromTriggerKind: OverlayControlKind? = null,
    ) {
        when (action) {
            OverlayAction.FastForward -> {
                if (fromTriggerKind == OverlayControlKind.ShoulderL2 ||
                    fromTriggerKind == OverlayControlKind.ShoulderR2
                ) {
                    // refreshTriggers owns L2/R2 → FF.
                    return
                }
                if (ffFromDigital != down) {
                    ffFromDigital = down
                    publishFastForward()
                }
            }
            OverlayAction.Menu -> if (down) onMenuClick()
            OverlayAction.ScreenSwap -> {
                if (fromTriggerKind == OverlayControlKind.ShoulderL2 ||
                    fromTriggerKind == OverlayControlKind.ShoulderR2
                ) {
                    // refreshTriggers owns L2/R2 → ScreenSwap edges.
                    return
                }
                if (down) onScreenSwap()
            }
            OverlayAction.ButtonA -> setButton(ControllerState.BUTTON_A, down)
            OverlayAction.ButtonB -> setButton(ControllerState.BUTTON_B, down)
            OverlayAction.ButtonX -> setButton(ControllerState.BUTTON_X, down)
            OverlayAction.ButtonY -> setButton(ControllerState.BUTTON_Y, down)
            OverlayAction.ButtonL -> setButton(ControllerState.BUTTON_LEFT_SHOULDER, down)
            OverlayAction.ButtonR -> setButton(ControllerState.BUTTON_RIGHT_SHOULDER, down)
            OverlayAction.ButtonL2 -> {
                if (fromTriggerKind != OverlayControlKind.ShoulderL2) {
                    leftTriggerButton = down
                }
            }
            OverlayAction.ButtonR2 -> {
                if (fromTriggerKind != OverlayControlKind.ShoulderR2) {
                    rightTriggerButton = down
                }
            }
            OverlayAction.Select -> setButton(ControllerState.BUTTON_BACK, down)
            OverlayAction.Start -> setButton(ControllerState.BUTTON_START, down)
            OverlayAction.LeftStick -> setButton(ControllerState.BUTTON_LEFT_STICK, down)
            OverlayAction.RightStick -> setButton(ControllerState.BUTTON_RIGHT_STICK, down)
            OverlayAction.Default -> Unit
        }
    }

    private fun refreshTriggers() {
        val l2 = actionFor(OverlayControlKind.ShoulderL2)
        val r2 = actionFor(OverlayControlKind.ShoulderR2)
        val l2Down = leftTriggerButton || leftTriggerAxis > 0.1f
        val r2Down = rightTriggerButton || rightTriggerAxis > 0.1f

        when (l2) {
            OverlayAction.FastForward -> {
                leftTrigger = 0
                if (ffFromL2 != l2Down) {
                    ffFromL2 = l2Down
                    publishFastForward()
                }
                if (swapFromL2) {
                    swapFromL2 = false
                }
            }
            OverlayAction.ScreenSwap -> {
                leftTrigger = 0
                if (ffFromL2) {
                    ffFromL2 = false
                    publishFastForward()
                }
                if (l2Down && !swapFromL2) {
                    swapFromL2 = true
                    onScreenSwap()
                } else if (!l2Down) {
                    swapFromL2 = false
                }
            }
            else -> {
                if (ffFromL2) {
                    ffFromL2 = false
                    publishFastForward()
                }
                swapFromL2 = false
                leftTrigger = if (l2 == OverlayAction.ButtonL2 || l2 == OverlayAction.Default) {
                    triggerLevel(leftTriggerButton, leftTriggerAxis)
                } else {
                    0
                }
            }
        }

        when (r2) {
            OverlayAction.FastForward -> {
                rightTrigger = 0
                if (ffFromR2 != r2Down) {
                    ffFromR2 = r2Down
                    publishFastForward()
                }
                if (swapFromR2) {
                    swapFromR2 = false
                }
            }
            OverlayAction.ScreenSwap -> {
                rightTrigger = 0
                if (ffFromR2) {
                    ffFromR2 = false
                    publishFastForward()
                }
                if (r2Down && !swapFromR2) {
                    swapFromR2 = true
                    onScreenSwap()
                } else if (!r2Down) {
                    swapFromR2 = false
                }
            }
            else -> {
                if (ffFromR2) {
                    ffFromR2 = false
                    publishFastForward()
                }
                swapFromR2 = false
                rightTrigger = if (r2 == OverlayAction.ButtonR2 || r2 == OverlayAction.Default) {
                    triggerLevel(rightTriggerButton, rightTriggerAxis)
                } else {
                    0
                }
            }
        }
    }

    private fun clearFastForward() {
        if (ffFromL2 || ffFromR2 || ffFromDigital) {
            ffFromL2 = false
            ffFromR2 = false
            ffFromDigital = false
            onFastForward(false)
        }
    }

    private fun publishFastForward() {
        onFastForward(ffFromL2 || ffFromR2 || ffFromDigital)
    }

    private fun emit() {
        onState(
            ControllerState(
                buttons = buttons,
                leftX = leftX,
                leftY = leftY,
                rightX = rightX,
                rightY = rightY,
                leftTrigger = leftTrigger,
                rightTrigger = rightTrigger,
            ),
        )
    }

    private fun setButton(mask: Int, down: Boolean) {
        if (mask == 0) return
        buttons = if (down) buttons or mask else buttons and mask.inv()
    }

    companion object {
        private const val DEADZONE = 0.18f

        private fun isMenuKey(keyCode: Int): Boolean =
            keyCode == KeyEvent.KEYCODE_BUTTON_MODE ||
                keyCode == KeyEvent.KEYCODE_HOME

        private fun buttonMask(keyCode: Int): Int = when (keyCode) {
            KeyEvent.KEYCODE_BUTTON_A -> ControllerState.BUTTON_A
            KeyEvent.KEYCODE_BUTTON_B -> ControllerState.BUTTON_B
            KeyEvent.KEYCODE_BUTTON_X -> ControllerState.BUTTON_X
            KeyEvent.KEYCODE_BUTTON_Y -> ControllerState.BUTTON_Y
            KeyEvent.KEYCODE_BUTTON_L1 -> ControllerState.BUTTON_LEFT_SHOULDER
            KeyEvent.KEYCODE_BUTTON_R1 -> ControllerState.BUTTON_RIGHT_SHOULDER
            KeyEvent.KEYCODE_BUTTON_THUMBL -> ControllerState.BUTTON_LEFT_STICK
            KeyEvent.KEYCODE_BUTTON_THUMBR -> ControllerState.BUTTON_RIGHT_STICK
            KeyEvent.KEYCODE_BUTTON_START -> ControllerState.BUTTON_START
            KeyEvent.KEYCODE_BUTTON_SELECT -> ControllerState.BUTTON_BACK
            KeyEvent.KEYCODE_DPAD_UP -> ControllerState.BUTTON_DPAD_UP
            KeyEvent.KEYCODE_DPAD_DOWN -> ControllerState.BUTTON_DPAD_DOWN
            KeyEvent.KEYCODE_DPAD_LEFT -> ControllerState.BUTTON_DPAD_LEFT
            KeyEvent.KEYCODE_DPAD_RIGHT -> ControllerState.BUTTON_DPAD_RIGHT
            else -> 0
        }

        private fun triggerLevel(buttonDown: Boolean, axis: Float): Int {
            if (buttonDown) return 0xFFFF
            if (axis <= 0.1f) return 0
            return (axis.coerceIn(0f, 1f) * 0xFFFF).roundToInt().coerceIn(0, 0xFFFF)
        }

        private fun axisToShort(value: Float): Short {
            val v = if (abs(value) < DEADZONE) 0f else value.coerceIn(-1f, 1f)
            return (v * Short.MAX_VALUE).roundToInt()
                .coerceIn(Short.MIN_VALUE.toInt(), Short.MAX_VALUE.toInt())
                .toShort()
        }

        private fun firstAxis(event: MotionEvent, primary: Int, fallback: Int): Float {
            val a = event.getAxisValue(primary)
            if (abs(a) > 0.01f) return a
            return event.getAxisValue(fallback)
        }

        private fun triggerAxis(event: MotionEvent, primary: Int, fallback: Int): Float {
            val a = event.getAxisValue(primary)
            if (a > 0.01f) return a.coerceIn(0f, 1f)
            val b = event.getAxisValue(fallback)
            return when {
                b > 0.01f -> b.coerceIn(0f, 1f)
                b < -0.01f -> ((b + 1f) * 0.5f).coerceIn(0f, 1f)
                else -> 0f
            }
        }
    }
}
