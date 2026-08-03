package com.archstreamer.client.ui

/**
 * On-screen pad layout driven by the game's system key (catalog systemKey).
 * Keeps handhelds readable: no dead stick/shoulder chrome on systems that don't use them.
 */
enum class PadLayout {
    /** Generic modern pad: left stick + d-pad + face + L/R + Select/Start. */
    Standard,

    /** Nintendo Switch: dual sticks + face + L/R/L2/R2 + Select/Start. */
    Switch,

    /** GBA-style: d-pad, A/B, L/R, Select/Start (no stick, no X/Y). */
    Gba,

    /** GB/GBC: d-pad, A/B, Select/Start only. */
    GameBoy,

    /** DS / 3DS-ish: d-pad, A/B/X/Y, L/R, R2 (swap), Select/Start. */
    DualScreen,
    ;

    companion object {
        fun forSystemKey(systemKey: String?): PadLayout {
            val key = systemKey.orEmpty().lowercase()
            return when {
                key == "gb" || key == "gbc" || key.contains("gameboy") -> GameBoy
                key == "gba" || key.contains("gameboyadvance") -> Gba
                key == "nds" || key == "3ds" || key == "n3ds" ||
                    key.contains("nintendo_ds") || key.contains("nintendo_3ds") -> DualScreen
                key == "switch" || key == "nsw" || key.contains("nintendo_switch") ||
                    key.contains("nintendo switch") -> Switch
                else -> Standard
            }
        }
    }
}
