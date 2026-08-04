package com.archstreamer.client.ui

import org.json.JSONArray
import org.json.JSONObject

/**
 * What a control does when pressed. [Default] resolves from [OverlayControlKind.defaultAction].
 * Remap e.g. R → R2 for melonDS screen swap.
 */
enum class OverlayAction(val id: String, val title: String) {
    Default("default", "Default"),
    ButtonA("a", "A"),
    ButtonB("b", "B"),
    ButtonX("x", "X"),
    ButtonY("y", "Y"),
    ButtonL("l", "L"),
    ButtonR("r", "R"),
    ButtonL2("l2", "L2"),
    ButtonR2("r2", "R2 (swap screens)"),
    Select("select", "Select"),
    Start("start", "Start"),
    Menu("menu", "Menu"),
    LeftStick("left_stick", "Left stick"),
    RightStick("right_stick", "Right stick"),
    FastForward("ff", "Fast-forward"),
    ;

    companion object {
        fun fromId(id: String): OverlayAction? = entries.firstOrNull { it.id == id }

        /** Actions assignable via the editor context menu (not Default). */
        val remappable: List<OverlayAction> = entries.filter { it != Default }
    }
}

/**
 * Grouped / atomic on-screen pad controls.
 * Clusters (dpad, face) move as one; Select and Start are separate remappable buttons.
 */
enum class OverlayControlKind(
    val id: String,
    val title: String,
    val defaultAction: OverlayAction,
    /** False for clusters whose internal buttons keep fixed mappings. */
    val remappable: Boolean,
) {
    Menu("menu", "Menu", OverlayAction.Menu, remappable = true),
    ShoulderL("shoulder_l", "L", OverlayAction.ButtonL, remappable = true),
    ShoulderR("shoulder_r", "R", OverlayAction.ButtonR, remappable = true),
    ShoulderL2("shoulder_l2", "L2", OverlayAction.ButtonL2, remappable = true),
    ShoulderR2("shoulder_r2", "R2", OverlayAction.ButtonR2, remappable = true),
    Select("select", "Select", OverlayAction.Select, remappable = true),
    Start("start", "Start", OverlayAction.Start, remappable = true),
    LeftStick("left_stick", "Left stick", OverlayAction.LeftStick, remappable = true),
    RightStick("right_stick", "Right stick", OverlayAction.RightStick, remappable = true),
    Dpad("dpad", "D-pad", OverlayAction.Default, remappable = false),
    FaceAbxy("face_abxy", "A / B / X / Y", OverlayAction.Default, remappable = false),
    FaceAb("face_ab", "A / B", OverlayAction.Default, remappable = false),
    FastForward("ff", "Fast-forward", OverlayAction.FastForward, remappable = true),
    ;

    companion object {
        fun fromId(id: String): OverlayControlKind? = when (id) {
            // Legacy grouped control — expanded in OverlayItemCodec.decode.
            "select_start" -> null
            else -> entries.firstOrNull { it.id == id }
        }
    }
}

/**
 * One placed control. [cx]/[cy] are normalized centers (0..1).
 * [action] defaults to the kind's natural mapping when [OverlayAction.Default].
 */
data class OverlayItem(
    val id: String,
    val kind: OverlayControlKind,
    val cx: Float,
    val cy: Float,
    val scale: Float = 1f,
    val action: OverlayAction = OverlayAction.Default,
) {
    fun resolvedAction(): OverlayAction =
        if (action == OverlayAction.Default) kind.defaultAction else action

    /** Label for chrome when remapped away from the kind default. */
    fun displayLabel(): String {
        val resolved = resolvedAction()
        if (!kind.remappable || action == OverlayAction.Default || resolved == kind.defaultAction) {
            return kind.title
        }
        return resolved.title.substringBefore(" (")
    }

    fun clamped(): OverlayItem = copy(
        cx = cx.coerceIn(0.02f, 0.98f),
        cy = cy.coerceIn(0.02f, 0.98f),
        scale = scale.coerceIn(0.55f, 1.75f),
    )
}

enum class OverlayOrientation(val id: String) {
    Landscape("landscape"),
    Portrait("portrait"),
    ;

    companion object {
        fun fromPortrait(isPortrait: Boolean): OverlayOrientation =
            if (isPortrait) Portrait else Landscape

        fun fromId(id: String): OverlayOrientation =
            entries.firstOrNull { it.id == id } ?: Landscape
    }
}

/** Built-in placements. */
object OverlayPresets {
    fun forLayout(layout: PadLayout): List<OverlayItem> = when (layout) {
        PadLayout.Standard -> listOf(
            item(OverlayControlKind.Menu, 0.055f, 0.065f),
            item(OverlayControlKind.ShoulderL, 0.12f, 0.09f),
            item(OverlayControlKind.Select, 0.42f, 0.09f),
            item(OverlayControlKind.Start, 0.58f, 0.09f),
            item(OverlayControlKind.ShoulderR, 0.88f, 0.09f),
            item(OverlayControlKind.LeftStick, 0.18f, 0.42f),
            item(OverlayControlKind.Dpad, 0.18f, 0.78f),
            item(OverlayControlKind.FaceAbxy, 0.82f, 0.72f),
        )
        PadLayout.Switch -> listOf(
            item(OverlayControlKind.Menu, 0.055f, 0.065f),
            item(OverlayControlKind.ShoulderL, 0.10f, 0.08f),
            item(OverlayControlKind.ShoulderL2, 0.22f, 0.08f),
            item(OverlayControlKind.Select, 0.42f, 0.09f),
            item(OverlayControlKind.Start, 0.58f, 0.09f),
            item(OverlayControlKind.ShoulderR2, 0.78f, 0.08f),
            item(OverlayControlKind.ShoulderR, 0.90f, 0.08f),
            item(OverlayControlKind.LeftStick, 0.16f, 0.38f),
            item(OverlayControlKind.Dpad, 0.16f, 0.78f),
            item(OverlayControlKind.RightStick, 0.84f, 0.38f),
            item(OverlayControlKind.FaceAbxy, 0.84f, 0.78f),
        )
        PadLayout.Gba -> listOf(
            item(OverlayControlKind.Menu, 0.055f, 0.065f),
            item(OverlayControlKind.ShoulderL, 0.12f, 0.09f),
            item(OverlayControlKind.Select, 0.42f, 0.09f),
            item(OverlayControlKind.Start, 0.58f, 0.09f),
            item(OverlayControlKind.ShoulderR, 0.88f, 0.09f),
            item(OverlayControlKind.Dpad, 0.18f, 0.72f),
            item(OverlayControlKind.FaceAb, 0.82f, 0.72f),
        )
        PadLayout.GameBoy -> listOf(
            item(OverlayControlKind.Menu, 0.055f, 0.065f),
            item(OverlayControlKind.Select, 0.42f, 0.09f),
            item(OverlayControlKind.Start, 0.58f, 0.09f),
            item(OverlayControlKind.Dpad, 0.18f, 0.72f),
            item(OverlayControlKind.FaceAb, 0.82f, 0.72f),
        )
        PadLayout.DualScreen -> listOf(
            item(OverlayControlKind.Menu, 0.055f, 0.065f),
            item(OverlayControlKind.ShoulderL, 0.10f, 0.08f),
            item(OverlayControlKind.Select, 0.42f, 0.09f),
            item(OverlayControlKind.Start, 0.58f, 0.09f),
            // R2 = melonDS "Swap screens" (same as desktop right trigger).
            item(OverlayControlKind.ShoulderR2, 0.78f, 0.08f),
            item(OverlayControlKind.ShoulderR, 0.90f, 0.08f),
            // Keep dpad/face on the top screen so the bottom pane stays tappable.
            // Landscape: left ~75% is top (EmphTop). Portrait stack: upper half is top.
            item(OverlayControlKind.Dpad, 0.16f, 0.40f),
            item(OverlayControlKind.FaceAbxy, 0.55f, 0.40f),
        )
    }

    fun spawnFor(kind: OverlayControlKind): OverlayItem = when (kind) {
        OverlayControlKind.Menu -> item(kind, 0.055f, 0.065f)
        OverlayControlKind.ShoulderL -> item(kind, 0.10f, 0.08f)
        OverlayControlKind.ShoulderL2 -> item(kind, 0.22f, 0.08f)
        OverlayControlKind.ShoulderR -> item(kind, 0.90f, 0.08f)
        OverlayControlKind.ShoulderR2 -> item(kind, 0.78f, 0.08f)
        OverlayControlKind.Select -> item(kind, 0.42f, 0.09f)
        OverlayControlKind.Start -> item(kind, 0.58f, 0.09f)
        OverlayControlKind.LeftStick -> item(kind, 0.16f, 0.38f)
        OverlayControlKind.RightStick -> item(kind, 0.84f, 0.38f)
        OverlayControlKind.Dpad -> item(kind, 0.18f, 0.78f)
        OverlayControlKind.FaceAbxy -> item(kind, 0.84f, 0.78f)
        OverlayControlKind.FaceAb -> item(kind, 0.82f, 0.72f)
        OverlayControlKind.FastForward -> item(kind, 0.50f, 0.22f)
    }

    private fun item(kind: OverlayControlKind, cx: Float, cy: Float, scale: Float = 1f) =
        OverlayItem(id = kind.id, kind = kind, cx = cx, cy = cy, scale = scale)
}

object OverlayItemCodec {
    fun encode(items: List<OverlayItem>): String {
        val arr = JSONArray()
        items.forEach { item ->
            arr.put(
                JSONObject()
                    .put("id", item.id)
                    .put("kind", item.kind.id)
                    .put("cx", item.cx.toDouble())
                    .put("cy", item.cy.toDouble())
                    .put("scale", item.scale.toDouble())
                    .put("action", item.action.id),
            )
        }
        return arr.toString()
    }

    fun decode(raw: String?): List<OverlayItem>? {
        if (raw.isNullOrBlank()) return null
        return runCatching {
            val arr = JSONArray(raw)
            buildList {
                for (i in 0 until arr.length()) {
                    val o = arr.getJSONObject(i)
                    val kindId = o.getString("kind")
                    // Legacy tray: expand into separate Select + Start.
                    if (kindId == "select_start") {
                        val cx = o.getDouble("cx").toFloat()
                        val cy = o.getDouble("cy").toFloat()
                        val scale = o.optDouble("scale", 1.0).toFloat()
                        add(
                            OverlayItem(
                                id = OverlayControlKind.Select.id,
                                kind = OverlayControlKind.Select,
                                cx = (cx - 0.06f).coerceIn(0.02f, 0.98f),
                                cy = cy,
                                scale = scale,
                            ).clamped(),
                        )
                        add(
                            OverlayItem(
                                id = OverlayControlKind.Start.id,
                                kind = OverlayControlKind.Start,
                                cx = (cx + 0.06f).coerceIn(0.02f, 0.98f),
                                cy = cy,
                                scale = scale,
                            ).clamped(),
                        )
                        continue
                    }
                    val kind = OverlayControlKind.fromId(kindId) ?: continue
                    val action = OverlayAction.fromId(o.optString("action", OverlayAction.Default.id))
                        ?: OverlayAction.Default
                    add(
                        OverlayItem(
                            id = o.optString("id", kind.id),
                            kind = kind,
                            cx = o.getDouble("cx").toFloat(),
                            cy = o.getDouble("cy").toFloat(),
                            scale = o.optDouble("scale", 1.0).toFloat(),
                            action = action,
                        ).clamped(),
                    )
                }
            }.ifEmpty { null }
        }.getOrNull()
    }
}
