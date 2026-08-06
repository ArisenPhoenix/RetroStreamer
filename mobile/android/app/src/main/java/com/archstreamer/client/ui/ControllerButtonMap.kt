package com.archstreamer.client.ui

import com.archstreamer.client.protocol.ControllerState
import org.json.JSONObject
import java.io.File

/**
 * Placement-free physical-controller remaps — Kotlin mirror of
 * `common/controller_button_map.hpp` + portable JSON document
 * (`shared/controller_button_map.json` / `controller_button_map.json` on disk).
 *
 * Stick axes are never remapped; only click bits (L3/R3) are.
 */
enum class ControllerMapFamily(val id: String, val title: String) {
    Standard("standard", "Standard"),
    Switch("switch", "Switch"),
    Gba("gba", "GBA"),
    Gb("gb", "GB / GBC"),
    Dual("dual", "DS / 3DS"),
    Psx("psx", "PSX"),
    ;

    companion object {
        fun fromId(id: String): ControllerMapFamily =
            entries.firstOrNull { it.id == id } ?: Standard

        fun fromSystemKey(systemKey: String?): ControllerMapFamily {
            val key = systemKey.orEmpty().lowercase()
            return when {
                key == "gb" || key == "gbc" || key.contains("gameboy") -> Gb
                key == "gba" || key.contains("gameboyadvance") -> Gba
                key == "nds" || key == "3ds" || key == "n3ds" ||
                    key.contains("nintendo_ds") || key.contains("nintendo_3ds") -> Dual
                key == "switch" || key == "nsw" || key.contains("nintendo_switch") ||
                    key.contains("nintendo switch") -> Switch
                key == "ps1" || key == "ps2" || key == "psp" || key == "psx" ||
                    key.contains("playstation") -> Psx
                else -> Standard
            }
        }
    }
}

enum class ControllerMapAction(val id: String, val title: String) {
    Default("default", "Default"),
    A("a", "A"),
    B("b", "B"),
    X("x", "X"),
    Y("y", "Y"),
    L("l", "L"),
    R("r", "R"),
    L2("l2", "L2"),
    R2("r2", "R2"),
    Select("select", "Select"),
    Start("start", "Start"),
    Menu("menu", "Menu"),
    LeftStick("left_stick", "L3"),
    RightStick("right_stick", "R3"),
    FastForward("ff", "Fast-forward"),
    ScreenSwap("screen_swap", "Screen swap"),
    ;

    companion object {
        fun fromId(id: String): ControllerMapAction =
            entries.firstOrNull { it.id == id } ?: Default

        /** Actions shown in remap dropdowns (includes Default). */
        val assignable: List<ControllerMapAction> = entries
    }
}

enum class ControllerMapSource(val id: String, val title: String) {
    Select("select", "Select"),
    Start("start", "Start"),
    L("l", "L"),
    R("r", "R"),
    L2("l2", "L2"),
    R2("r2", "R2"),
    L3("l3", "L3"),
    R3("r3", "R3"),
    ;

    companion object {
        fun fromId(id: String): ControllerMapSource? = when (id) {
            "select" -> Select
            "start" -> Start
            "l", "shoulder_l" -> L
            "r", "shoulder_r" -> R
            "l2", "shoulder_l2" -> L2
            "r2", "shoulder_r2" -> R2
            "l3", "left_stick" -> L3
            "r3", "right_stick" -> R3
            else -> null
        }
    }
}

data class ControllerMapProfile(
    val swapNw: Boolean = false,
    val swapSe: Boolean = false,
    val select: ControllerMapAction = ControllerMapAction.Default,
    val start: ControllerMapAction = ControllerMapAction.Default,
    val l: ControllerMapAction = ControllerMapAction.Default,
    val r: ControllerMapAction = ControllerMapAction.Default,
    val l2: ControllerMapAction = ControllerMapAction.Default,
    val r2: ControllerMapAction = ControllerMapAction.Default,
    val l3: ControllerMapAction = ControllerMapAction.Default,
    val r3: ControllerMapAction = ControllerMapAction.Default,
) {
    fun actionFor(source: ControllerMapSource): ControllerMapAction = when (source) {
        ControllerMapSource.Select -> select
        ControllerMapSource.Start -> start
        ControllerMapSource.L -> l
        ControllerMapSource.R -> r
        ControllerMapSource.L2 -> l2
        ControllerMapSource.R2 -> r2
        ControllerMapSource.L3 -> l3
        ControllerMapSource.R3 -> r3
    }

    fun withAction(source: ControllerMapSource, action: ControllerMapAction): ControllerMapProfile =
        when (source) {
            ControllerMapSource.Select -> copy(select = action)
            ControllerMapSource.Start -> copy(start = action)
            ControllerMapSource.L -> copy(l = action)
            ControllerMapSource.R -> copy(r = action)
            ControllerMapSource.L2 -> copy(l2 = action)
            ControllerMapSource.R2 -> copy(r2 = action)
            ControllerMapSource.L3 -> copy(l3 = action)
            ControllerMapSource.R3 -> copy(r3 = action)
        }

    fun identity(): Boolean =
        !swapNw && !swapSe &&
            select == ControllerMapAction.Default &&
            start == ControllerMapAction.Default &&
            l == ControllerMapAction.Default &&
            r == ControllerMapAction.Default &&
            l2 == ControllerMapAction.Default &&
            r2 == ControllerMapAction.Default &&
            l3 == ControllerMapAction.Default &&
            r3 == ControllerMapAction.Default

    companion object {
        val DEFAULT = ControllerMapProfile()
    }
}

data class ControllerMapDocument(
    val version: Int = DOCUMENT_VERSION,
    val profiles: Map<ControllerMapFamily, ControllerMapProfile> =
        ControllerMapFamily.entries.associateWith { ControllerMapProfile.DEFAULT },
) {
    fun profile(family: ControllerMapFamily): ControllerMapProfile =
        profiles[family] ?: ControllerMapProfile.DEFAULT

    fun withProfile(family: ControllerMapFamily, profile: ControllerMapProfile): ControllerMapDocument =
        copy(profiles = profiles + (family to profile))

    companion object {
        const val DOCUMENT_VERSION = 1
        const val FILE_NAME = "controller_button_map.json"
    }
}

data class ControllerMapApplyState(
    var prevMenuDown: Boolean = false,
    var prevFfHeld: Boolean = false,
    var prevScreenSwapDown: Boolean = false,
)

data class ControllerMapApplyExtras(
    var fastForwardHeld: Boolean = false,
    var menuEdge: Boolean = false,
    var fastForwardChanged: Boolean = false,
    var screenSwapEdge: Boolean = false,
)

object ControllerButtonMap {
    private const val TRIGGER_DOWN_THRESHOLD = 6554 // ~0.1 * 65535

    fun defaultAction(source: ControllerMapSource): ControllerMapAction = when (source) {
        ControllerMapSource.Select -> ControllerMapAction.Select
        ControllerMapSource.Start -> ControllerMapAction.Start
        ControllerMapSource.L -> ControllerMapAction.L
        ControllerMapSource.R -> ControllerMapAction.R
        ControllerMapSource.L2 -> ControllerMapAction.L2
        ControllerMapSource.R2 -> ControllerMapAction.R2
        ControllerMapSource.L3 -> ControllerMapAction.LeftStick
        ControllerMapSource.R3 -> ControllerMapAction.RightStick
    }

    fun resolveAction(source: ControllerMapSource, stored: ControllerMapAction): ControllerMapAction =
        if (stored == ControllerMapAction.Default) defaultAction(source) else stored

    fun apply(
        state: ControllerState,
        profile: ControllerMapProfile,
        applyState: ControllerMapApplyState,
    ): Pair<ControllerState, ControllerMapApplyExtras> {
        val extras = ControllerMapApplyExtras()
        if (profile.identity()) {
            applyState.prevMenuDown = false
            applyState.prevScreenSwapDown = false
            if (applyState.prevFfHeld) {
                extras.fastForwardChanged = true
            }
            applyState.prevFfHeld = false
            return state to extras
        }

        val selectDown = sourceDown(state, ControllerMapSource.Select)
        val startDown = sourceDown(state, ControllerMapSource.Start)
        val lDown = sourceDown(state, ControllerMapSource.L)
        val rDown = sourceDown(state, ControllerMapSource.R)
        val l2Down = sourceDown(state, ControllerMapSource.L2)
        val r2Down = sourceDown(state, ControllerMapSource.R2)
        val l3Down = sourceDown(state, ControllerMapSource.L3)
        val r3Down = sourceDown(state, ControllerMapSource.R3)
        val l2Level = sourceAnalogLevel(state, ControllerMapSource.L2)
        val r2Level = sourceAnalogLevel(state, ControllerMapSource.R2)

        var buttons = state.buttons and
            (
                ControllerState.BUTTON_BACK or
                    ControllerState.BUTTON_START or
                    ControllerState.BUTTON_LEFT_SHOULDER or
                    ControllerState.BUTTON_RIGHT_SHOULDER or
                    ControllerState.BUTTON_LEFT_STICK or
                    ControllerState.BUTTON_RIGHT_STICK
                ).inv()
        var leftTrigger = 0
        var rightTrigger = 0
        var ffHeld = false
        var menuDown = false
        var screenSwapDown = false

        fun dispatch(source: ControllerMapSource, down: Boolean, level: Int) {
            if (!down) return
            when (resolveAction(source, profile.actionFor(source))) {
                ControllerMapAction.Default -> Unit
                ControllerMapAction.A -> buttons = buttons or ControllerState.BUTTON_A
                ControllerMapAction.B -> buttons = buttons or ControllerState.BUTTON_B
                ControllerMapAction.X -> buttons = buttons or ControllerState.BUTTON_X
                ControllerMapAction.Y -> buttons = buttons or ControllerState.BUTTON_Y
                ControllerMapAction.L -> buttons = buttons or ControllerState.BUTTON_LEFT_SHOULDER
                ControllerMapAction.R -> buttons = buttons or ControllerState.BUTTON_RIGHT_SHOULDER
                ControllerMapAction.L2 -> leftTrigger = maxOf(leftTrigger, level)
                ControllerMapAction.R2 -> rightTrigger = maxOf(rightTrigger, level)
                ControllerMapAction.Select -> buttons = buttons or ControllerState.BUTTON_BACK
                ControllerMapAction.Start -> buttons = buttons or ControllerState.BUTTON_START
                ControllerMapAction.Menu -> menuDown = true
                ControllerMapAction.LeftStick -> buttons = buttons or ControllerState.BUTTON_LEFT_STICK
                ControllerMapAction.RightStick -> buttons = buttons or ControllerState.BUTTON_RIGHT_STICK
                ControllerMapAction.FastForward -> ffHeld = true
                ControllerMapAction.ScreenSwap -> screenSwapDown = true
            }
        }

        dispatch(ControllerMapSource.Select, selectDown, 0xFFFF)
        dispatch(ControllerMapSource.Start, startDown, 0xFFFF)
        dispatch(ControllerMapSource.L, lDown, 0xFFFF)
        dispatch(ControllerMapSource.R, rDown, 0xFFFF)
        dispatch(ControllerMapSource.L2, l2Down, if (l2Level == 0) 0xFFFF else l2Level)
        dispatch(ControllerMapSource.R2, r2Down, if (r2Level == 0) 0xFFFF else r2Level)
        dispatch(ControllerMapSource.L3, l3Down, 0xFFFF)
        dispatch(ControllerMapSource.R3, r3Down, 0xFFFF)

        var out = state.copy(
            buttons = buttons,
            leftTrigger = leftTrigger,
            rightTrigger = rightTrigger,
        )
        out = ControllerState.applyFaceButtonSwaps(out, profile.swapNw, profile.swapSe)

        extras.fastForwardHeld = ffHeld
        extras.menuEdge = menuDown && !applyState.prevMenuDown
        extras.fastForwardChanged = ffHeld != applyState.prevFfHeld
        extras.screenSwapEdge = screenSwapDown && !applyState.prevScreenSwapDown
        applyState.prevMenuDown = menuDown
        applyState.prevFfHeld = ffHeld
        applyState.prevScreenSwapDown = screenSwapDown
        return out to extras
    }

    private fun sourceDown(state: ControllerState, source: ControllerMapSource): Boolean =
        when (source) {
            ControllerMapSource.Select -> state.buttons and ControllerState.BUTTON_BACK != 0
            ControllerMapSource.Start -> state.buttons and ControllerState.BUTTON_START != 0
            ControllerMapSource.L -> state.buttons and ControllerState.BUTTON_LEFT_SHOULDER != 0
            ControllerMapSource.R -> state.buttons and ControllerState.BUTTON_RIGHT_SHOULDER != 0
            ControllerMapSource.L2 -> state.leftTrigger > TRIGGER_DOWN_THRESHOLD
            ControllerMapSource.R2 -> state.rightTrigger > TRIGGER_DOWN_THRESHOLD
            ControllerMapSource.L3 -> state.buttons and ControllerState.BUTTON_LEFT_STICK != 0
            ControllerMapSource.R3 -> state.buttons and ControllerState.BUTTON_RIGHT_STICK != 0
        }

    private fun sourceAnalogLevel(state: ControllerState, source: ControllerMapSource): Int =
        when (source) {
            ControllerMapSource.L2 -> state.leftTrigger
            ControllerMapSource.R2 -> state.rightTrigger
            else -> 0xFFFF
        }
}

/** Load / save the portable `controller_button_map.json` document. */
object ControllerMapStore {
    fun load(file: File): ControllerMapDocument {
        if (!file.isFile) return ControllerMapDocument()
        return runCatching { decode(file.readText()) }.getOrDefault(ControllerMapDocument())
    }

    fun save(file: File, document: ControllerMapDocument) {
        file.parentFile?.mkdirs()
        file.writeText(encode(document))
    }

    fun encode(document: ControllerMapDocument): String {
        val families = JSONObject()
        ControllerMapFamily.entries.forEach { family ->
            families.put(family.id, encodeProfile(document.profile(family)))
        }
        return JSONObject()
            .put("version", document.version)
            .put("families", families)
            .toString(2)
    }

    fun decode(json: String): ControllerMapDocument {
        val root = JSONObject(json)
        val familiesObj = root.optJSONObject("families") ?: JSONObject()
        val profiles = ControllerMapFamily.entries.associateWith { family ->
            val obj = familiesObj.optJSONObject(family.id)
            if (obj != null) decodeProfile(obj) else ControllerMapProfile.DEFAULT
        }
        return ControllerMapDocument(
            version = root.optInt("version", ControllerMapDocument.DOCUMENT_VERSION),
            profiles = profiles,
        )
    }

    private fun encodeProfile(profile: ControllerMapProfile): JSONObject =
        JSONObject()
            .put("swap_nw", profile.swapNw)
            .put("swap_se", profile.swapSe)
            .put("select", profile.select.id)
            .put("start", profile.start.id)
            .put("l", profile.l.id)
            .put("r", profile.r.id)
            .put("l2", profile.l2.id)
            .put("r2", profile.r2.id)
            .put("l3", profile.l3.id)
            .put("r3", profile.r3.id)

    private fun decodeProfile(obj: JSONObject): ControllerMapProfile =
        ControllerMapProfile(
            swapNw = obj.optBoolean("swap_nw", false),
            swapSe = obj.optBoolean("swap_se", false),
            select = ControllerMapAction.fromId(obj.optString("select", "default")),
            start = ControllerMapAction.fromId(obj.optString("start", "default")),
            l = ControllerMapAction.fromId(obj.optString("l", "default")),
            r = ControllerMapAction.fromId(obj.optString("r", "default")),
            l2 = ControllerMapAction.fromId(obj.optString("l2", "default")),
            r2 = ControllerMapAction.fromId(obj.optString("r2", "default")),
            l3 = ControllerMapAction.fromId(obj.optString("l3", "default")),
            r3 = ControllerMapAction.fromId(obj.optString("r3", "default")),
        )
}
