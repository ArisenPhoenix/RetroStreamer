package com.archstreamer.client.ui

import android.content.SharedPreferences
import org.json.JSONArray
import org.json.JSONObject
import kotlin.math.roundToInt

/**
 * Per-system overlay chrome family. Defaults match [PadLayout.forSystemKey];
 * Game Options can override layout, face swaps, opacity, and one named custom layout
 * with separate landscape / portrait placements.
 */
enum class OverlaySystemFamily(val id: String, val title: String) {
    Standard("standard", "Standard"),
    Switch("switch", "Switch"),
    Gba("gba", "GBA"),
    Gb("gb", "GB / GBC"),
    Dual("dual", "DS / 3DS"),
    Psx("psx", "PSX"),
    ;

    companion object {
        fun fromSystemKey(systemKey: String?): OverlaySystemFamily =
            fromId(ControllerMapFamily.fromSystemKey(systemKey).id)

        fun fromId(id: String): OverlaySystemFamily =
            entries.firstOrNull { it.id == id } ?: Standard

        fun toMapFamily(family: OverlaySystemFamily): ControllerMapFamily =
            ControllerMapFamily.fromId(family.id)
    }
}

/**
 * Layout choice inside a profile.
 * [Custom] uses the single stored [OverlayProfile.custom] slot when present.
 */
enum class OverlayLayoutMode(val id: String, val title: String) {
    Auto("auto", "Auto"),
    Standard("standard", "Standard"),
    Switch("switch", "Switch"),
    Gba("gba", "GBA"),
    GameBoy("gb", "GB / GBC"),
    DualScreen("dual", "DS / 3DS"),
    Custom("custom", "Custom"),
    ;

    val isBuiltin: Boolean get() = this != Custom

    fun toPadLayout(systemKey: String?): PadLayout = when (this) {
        Auto, Custom -> PadLayout.forSystemKey(systemKey)
        Standard -> PadLayout.Standard
        Switch -> PadLayout.Switch
        Gba -> PadLayout.Gba
        GameBoy -> PadLayout.GameBoy
        DualScreen -> PadLayout.DualScreen
    }

    companion object {
        fun fromId(id: String): OverlayLayoutMode =
            entries.firstOrNull { it.id == id } ?: Auto

        val builtins: List<OverlayLayoutMode> = entries.filter { it.isBuiltin }
    }
}

/**
 * One named custom pad per family, with independent landscape / portrait item lists
 * (presence + coords + actions can differ per orientation).
 */
data class OverlayCustomLayout(
    val name: String,
    val landscape: List<OverlayItem> = emptyList(),
    val portrait: List<OverlayItem> = emptyList(),
) {
    fun clampedName(): String =
        name.trim().ifBlank { DEFAULT_NAME }.take(MAX_NAME_LEN)

    fun itemsFor(orientation: OverlayOrientation): List<OverlayItem> = when (orientation) {
        OverlayOrientation.Landscape -> landscape.ifEmpty { portrait }
        OverlayOrientation.Portrait -> portrait.ifEmpty { landscape }
    }

    fun withItems(orientation: OverlayOrientation, items: List<OverlayItem>): OverlayCustomLayout =
        when (orientation) {
            OverlayOrientation.Landscape -> copy(landscape = items)
            OverlayOrientation.Portrait -> copy(portrait = items)
        }

    companion object {
        const val DEFAULT_NAME = "Custom"
        const val MAX_NAME_LEN = 24
    }
}

data class OverlayProfile(
    val layoutMode: OverlayLayoutMode = OverlayLayoutMode.Auto,
    val swapNw: Boolean = false,
    val swapSe: Boolean = false,
    val opacity: Float = DEFAULT_OPACITY,
    val custom: OverlayCustomLayout? = null,
) {
    fun resolveLayout(systemKey: String?): PadLayout = layoutMode.toPadLayout(systemKey)

    fun resolveItems(
        systemKey: String?,
        orientation: OverlayOrientation = OverlayOrientation.Landscape,
    ): List<OverlayItem> = when (layoutMode) {
        OverlayLayoutMode.Custom -> {
            val customItems = custom?.itemsFor(orientation)
            if (!customItems.isNullOrEmpty()) customItems
            else OverlayPresets.forLayout(resolveLayout(systemKey))
        }
        else -> OverlayPresets.forLayout(resolveLayout(systemKey))
    }

    fun clampedOpacity(): Float = opacity.coerceIn(MIN_OPACITY, MAX_OPACITY)

    companion object {
        const val DEFAULT_OPACITY = 0.90f
        const val MIN_OPACITY = 0.25f
        const val MAX_OPACITY = 1.0f

        val DEFAULT = OverlayProfile()
    }
}

/** Load / save overlay.<family>.* prefs (runtime cache; SQL is canonical). */
object OverlayProfileStore {
    const val DOCUMENT_VERSION = 1

    fun loadAll(prefs: SharedPreferences): Map<OverlaySystemFamily, OverlayProfile> =
        OverlaySystemFamily.entries.associateWith { load(prefs, it) }

    fun load(prefs: SharedPreferences, family: OverlaySystemFamily): OverlayProfile {
        val prefix = "overlay.${family.id}."
        val layoutStored = OverlayLayoutMode.fromId(
            prefs.getString(prefix + "layout", OverlayLayoutMode.Auto.id) ?: OverlayLayoutMode.Auto.id,
        )
        val swapNw = prefs.getBoolean(prefix + "swap_nw", false)
        val swapSe = prefs.getBoolean(prefix + "swap_se", false)
        val opacityRaw = prefs.getFloat(prefix + "opacity", OverlayProfile.DEFAULT_OPACITY)

        val landscape = OverlayItemCodec.decode(prefs.getString(prefix + "items_landscape", null))
            ?: OverlayItemCodec.decode(prefs.getString(prefix + "items", null))
        val portrait = OverlayItemCodec.decode(prefs.getString(prefix + "items_portrait", null))
            ?: landscape
        val hasCustomName = prefs.contains(prefix + "custom_name")
        val custom = if (landscape != null || portrait != null) {
            OverlayCustomLayout(
                name = prefs.getString(prefix + "custom_name", null)
                    ?: OverlayCustomLayout.DEFAULT_NAME,
                landscape = landscape.orEmpty(),
                portrait = portrait.orEmpty(),
            )
        } else {
            null
        }
        val layout = when {
            custom != null && !hasCustomName && layoutStored != OverlayLayoutMode.Custom ->
                OverlayLayoutMode.Custom
            layoutStored == OverlayLayoutMode.Custom && custom == null ->
                OverlayLayoutMode.Auto
            else -> layoutStored
        }
        return OverlayProfile(
            layoutMode = layout,
            swapNw = swapNw,
            swapSe = swapSe,
            opacity = opacityRaw.coerceIn(OverlayProfile.MIN_OPACITY, OverlayProfile.MAX_OPACITY),
            custom = custom,
        )
    }

    fun save(prefs: SharedPreferences, family: OverlaySystemFamily, profile: OverlayProfile) {
        val prefix = "overlay.${family.id}."
        val editor = prefs.edit()
            .putString(prefix + "layout", profile.layoutMode.id)
            .putBoolean(prefix + "swap_nw", profile.swapNw)
            .putBoolean(prefix + "swap_se", profile.swapSe)
            .putFloat(prefix + "opacity", profile.clampedOpacity())
        val custom = profile.custom
        if (custom == null) {
            editor.remove(prefix + "items")
            editor.remove(prefix + "items_landscape")
            editor.remove(prefix + "items_portrait")
            editor.remove(prefix + "custom_name")
        } else {
            editor.putString(prefix + "custom_name", custom.clampedName())
            editor.putString(prefix + "items_landscape", OverlayItemCodec.encode(custom.landscape))
            editor.putString(prefix + "items_portrait", OverlayItemCodec.encode(custom.portrait))
            // Legacy single blob = landscape (older builds).
            editor.putString(prefix + "items", OverlayItemCodec.encode(custom.landscape))
        }
        editor.apply()
    }

    fun saveAll(prefs: SharedPreferences, profiles: Map<OverlaySystemFamily, OverlayProfile>) {
        profiles.forEach { (family, profile) -> save(prefs, family, profile) }
    }

    fun reset(prefs: SharedPreferences, family: OverlaySystemFamily) {
        val prefix = "overlay.${family.id}."
        prefs.edit()
            .remove(prefix + "layout")
            .remove(prefix + "swap_nw")
            .remove(prefix + "swap_se")
            .remove(prefix + "opacity")
            .remove(prefix + "items")
            .remove(prefix + "items_landscape")
            .remove(prefix + "items_portrait")
            .remove(prefix + "custom_name")
            .apply()
    }

    /** Encode all families into the overlay_profiles SQL document JSON. */
    fun encodeDocument(profiles: Map<OverlaySystemFamily, OverlayProfile>): String {
        val families = JSONObject()
        OverlaySystemFamily.entries.forEach { family ->
            val profile = profiles[family] ?: OverlayProfile.DEFAULT
            val obj = JSONObject()
                .put("layout", profile.layoutMode.id)
                .put("opacity", profile.clampedOpacity().toDouble())
                .put("swap_nw", profile.swapNw)
                .put("swap_se", profile.swapSe)
            val custom = profile.custom
            if (custom != null) {
                obj.put("custom_name", custom.clampedName())
                obj.put("items_landscape", JSONArray(OverlayItemCodec.encode(custom.landscape)))
                obj.put("items_portrait", JSONArray(OverlayItemCodec.encode(custom.portrait)))
            }
            families.put(family.id, obj)
        }
        return JSONObject()
            .put("version", DOCUMENT_VERSION)
            .put("families", families)
            .toString()
    }

    fun decodeDocument(raw: String?): Map<OverlaySystemFamily, OverlayProfile>? {
        if (raw.isNullOrBlank()) return null
        return runCatching {
            val root = JSONObject(raw)
            val families = root.optJSONObject("families") ?: return@runCatching null
            OverlaySystemFamily.entries.associateWith { family ->
                val obj = families.optJSONObject(family.id) ?: return@associateWith OverlayProfile.DEFAULT
                val landscape = OverlayItemCodec.decode(obj.optJSONArray("items_landscape")?.toString())
                val portrait = OverlayItemCodec.decode(obj.optJSONArray("items_portrait")?.toString())
                    ?: landscape
                val custom = if (landscape != null || portrait != null) {
                    OverlayCustomLayout(
                        name = obj.optString("custom_name", OverlayCustomLayout.DEFAULT_NAME),
                        landscape = landscape.orEmpty(),
                        portrait = portrait.orEmpty(),
                    )
                } else {
                    null
                }
                var layout = OverlayLayoutMode.fromId(obj.optString("layout", OverlayLayoutMode.Auto.id))
                if (layout == OverlayLayoutMode.Custom && custom == null) {
                    layout = OverlayLayoutMode.Auto
                }
                OverlayProfile(
                    layoutMode = layout,
                    swapNw = obj.optBoolean("swap_nw", false),
                    swapSe = obj.optBoolean("swap_se", false),
                    opacity = obj.optDouble("opacity", OverlayProfile.DEFAULT_OPACITY.toDouble())
                        .toFloat()
                        .coerceIn(OverlayProfile.MIN_OPACITY, OverlayProfile.MAX_OPACITY),
                    custom = custom,
                )
            }
        }.getOrNull()
    }
}

fun opacityToAlpha(opacity: Float): Int =
    (opacity.coerceIn(0f, 1f) * 255f).roundToInt().coerceIn(0, 255)
