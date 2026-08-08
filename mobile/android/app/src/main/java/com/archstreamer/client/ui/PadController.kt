package com.archstreamer.client.ui

import com.archstreamer.client.protocol.ControllerState

/**
 * Logical pad controls. Remappable sources mirror [ControllerMapSource];
 * Menu / FastForward / ScreenSwap are overlay-capable extras (physical may not support them).
 */
enum class PadControl {
    Select,
    Start,
    L,
    R,
    L2,
    R2,
    L3,
    R3,
    Menu,
    FastForward,
    ScreenSwap,
    ;

    fun toMapSource(): ControllerMapSource? = when (this) {
        Select -> ControllerMapSource.Select
        Start -> ControllerMapSource.Start
        L -> ControllerMapSource.L
        R -> ControllerMapSource.R
        L2 -> ControllerMapSource.L2
        R2 -> ControllerMapSource.R2
        L3 -> ControllerMapSource.L3
        R3 -> ControllerMapSource.R3
        Menu, FastForward, ScreenSwap -> null
    }

    companion object {
        fun fromMapSource(source: ControllerMapSource): PadControl = when (source) {
            ControllerMapSource.Select -> Select
            ControllerMapSource.Start -> Start
            ControllerMapSource.L -> L
            ControllerMapSource.R -> R
            ControllerMapSource.L2 -> L2
            ControllerMapSource.R2 -> R2
            ControllerMapSource.L3 -> L3
            ControllerMapSource.R3 -> R3
        }

        fun fromOverlayKind(kind: OverlayControlKind): PadControl? = when (kind) {
            OverlayControlKind.Select -> Select
            OverlayControlKind.Start -> Start
            OverlayControlKind.ShoulderL -> L
            OverlayControlKind.ShoulderR -> R
            OverlayControlKind.ShoulderL2 -> L2
            OverlayControlKind.ShoulderR2 -> R2
            OverlayControlKind.LeftStick -> L3
            OverlayControlKind.RightStick -> R3
            OverlayControlKind.Menu -> Menu
            OverlayControlKind.FastForward -> FastForward
            OverlayControlKind.ScreenSwap -> ScreenSwap
            OverlayControlKind.Dpad,
            OverlayControlKind.FaceAbxy,
            OverlayControlKind.FaceAb,
            -> null
        }
    }
}

/**
 * Thin input surface over the shared [MappedPadPipeline].
 * Physical and overlay both implement this; keyboard/TV-remote stay outside.
 */
interface PadController {
    fun supports(control: PadControl): Boolean
    fun dispatch(control: PadControl, down: Boolean): Boolean
    fun ingestFixed(state: ControllerState)
    fun reset()
}

fun ControllerMapAction.toOverlayAction(): OverlayAction = when (this) {
    ControllerMapAction.Default -> OverlayAction.Default
    ControllerMapAction.A -> OverlayAction.ButtonA
    ControllerMapAction.B -> OverlayAction.ButtonB
    ControllerMapAction.X -> OverlayAction.ButtonX
    ControllerMapAction.Y -> OverlayAction.ButtonY
    ControllerMapAction.L -> OverlayAction.ButtonL
    ControllerMapAction.R -> OverlayAction.ButtonR
    ControllerMapAction.L2 -> OverlayAction.ButtonL2
    ControllerMapAction.R2 -> OverlayAction.ButtonR2
    ControllerMapAction.Select -> OverlayAction.Select
    ControllerMapAction.Start -> OverlayAction.Start
    ControllerMapAction.Menu -> OverlayAction.Menu
    ControllerMapAction.LeftStick -> OverlayAction.LeftStick
    ControllerMapAction.RightStick -> OverlayAction.RightStick
    ControllerMapAction.FastForward -> OverlayAction.FastForward
    ControllerMapAction.ScreenSwap -> OverlayAction.ScreenSwap
}

/**
 * Single remap + face-swap pipeline. Physical and overlay both resolve remappable
 * sources (Select/Start/L/R/L2/R2/L3/R3) through [map] — the Cadence button_map doc.
 */
class MappedPadPipeline(
    private val map: () -> ControllerMapProfile,
    private val sink: (ControllerState) -> Unit,
    private val onMenu: () -> Unit,
    private val onFfHold: (Boolean) -> Unit,
    private val onScreenSwap: () -> Unit,
) {
    fun profile(): ControllerMapProfile = map()

    /** Resolved action for a remappable source (Default → natural binding). */
    fun resolve(source: ControllerMapSource): ControllerMapAction =
        ControllerButtonMap.resolveAction(source, profile().actionFor(source))

    fun resolveOverlayAction(source: ControllerMapSource): OverlayAction =
        resolve(source).toOverlayAction()

    /**
     * Play-time action for an overlay chrome item:
     * remappable map sources → [ControllerMapProfile]; overlay-only Menu/FF/ScreenSwap → item action.
     * Stick axis Left↔Right swap stays on the item (placement chrome, not L3/R3 click map).
     */
    fun playOverlayAction(item: OverlayItem): OverlayAction {
        val source = when (item.kind) {
            OverlayControlKind.Select -> ControllerMapSource.Select
            OverlayControlKind.Start -> ControllerMapSource.Start
            OverlayControlKind.ShoulderL -> ControllerMapSource.L
            OverlayControlKind.ShoulderR -> ControllerMapSource.R
            OverlayControlKind.ShoulderL2 -> ControllerMapSource.L2
            OverlayControlKind.ShoulderR2 -> ControllerMapSource.R2
            else -> null
        }
        if (source != null) {
            return resolveOverlayAction(source)
        }
        return item.resolvedAction()
    }

    fun publishWithSwaps(raw: ControllerState) {
        val p = profile()
        sink(ControllerState.applyFaceButtonSwaps(raw, p.swapNw, p.swapSe))
    }

    fun notifyMenu() = onMenu()
    fun notifyFfHold(held: Boolean) = onFfHold(held)
    fun notifyScreenSwap() = onScreenSwap()
}

/**
 * Physical pad adapter: KeyEvent/MotionEvent → shared remap pipeline.
 * Remappable keys use [MappedPadPipeline.resolve]; face/dpad stay fixed bits.
 */
class PhysicalPadController(
    private val pipeline: MappedPadPipeline,
) : PadController {
    private val tracker = PhysicalGamepadTracker(
        actionFor = { kind ->
            val source = PadControl.fromOverlayKind(kind)?.toMapSource()
            if (source != null) {
                pipeline.resolveOverlayAction(source)
            } else {
                kind.defaultAction
            }
        },
        onState = { raw -> pipeline.publishWithSwaps(raw) },
        onMenuClick = { pipeline.notifyMenu() },
        onFastForward = { pipeline.notifyFfHold(it) },
        onScreenSwap = { pipeline.notifyScreenSwap() },
    )

    val gamepadTracker: PhysicalGamepadTracker get() = tracker

    override fun supports(control: PadControl): Boolean =
        control.toMapSource() != null || control == PadControl.Menu

    override fun dispatch(control: PadControl, down: Boolean): Boolean {
        if (!supports(control)) return false
        val source = control.toMapSource()
        if (source != null) {
            val kind = when (source) {
                ControllerMapSource.Select -> OverlayControlKind.Select
                ControllerMapSource.Start -> OverlayControlKind.Start
                ControllerMapSource.L -> OverlayControlKind.ShoulderL
                ControllerMapSource.R -> OverlayControlKind.ShoulderR
                ControllerMapSource.L2 -> OverlayControlKind.ShoulderL2
                ControllerMapSource.R2 -> OverlayControlKind.ShoulderR2
                ControllerMapSource.L3 -> OverlayControlKind.LeftStick
                ControllerMapSource.R3 -> OverlayControlKind.RightStick
            }
            // Re-enter via tracker path is awkward; physical input comes from KeyEvents.
            // dispatch is for capability tests / future synthetic inject.
            tracker.handleSyntheticRemappable(kind, down)
            return true
        }
        if (control == PadControl.Menu && down) {
            pipeline.notifyMenu()
            return true
        }
        return false
    }

    override fun ingestFixed(state: ControllerState) {
        pipeline.publishWithSwaps(state)
    }

    override fun reset() = tracker.reset()
}

/**
 * Overlay pad adapter: Compose presses → shared remap for shoulders/select/start;
 * Menu/FF/ScreenSwap chrome and stick axes stay overlay-capable.
 */
class OverlayPadController(
    private val pipeline: MappedPadPipeline,
) : PadController {
    override fun supports(control: PadControl): Boolean = true

    override fun dispatch(control: PadControl, down: Boolean): Boolean {
        val action = when (val source = control.toMapSource()) {
            null -> when (control) {
                PadControl.Menu -> OverlayAction.Menu
                PadControl.FastForward -> OverlayAction.FastForward
                PadControl.ScreenSwap -> OverlayAction.ScreenSwap
                else -> return false
            }
            else -> pipeline.resolveOverlayAction(source)
        }
        // Overlay Compose path applies via GamepadOverlay.applyAction; this is the
        // shared semantic entry for tests / future non-Compose inject.
        applyOverlayAction(action, down)
        return true
    }

    private var applyOverlayAction: (OverlayAction, Boolean) -> Unit = { _, _ -> }

    fun bindActionApplier(applier: (OverlayAction, Boolean) -> Unit) {
        applyOverlayAction = applier
    }

    override fun ingestFixed(state: ControllerState) {
        pipeline.publishWithSwaps(state)
    }

    override fun reset() = Unit

    fun playActionFor(item: OverlayItem): OverlayAction = pipeline.playOverlayAction(item)
}
