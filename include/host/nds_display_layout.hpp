#pragma once

#include "common/protocol.hpp"

namespace archstreamer {

/**
 * Shared NDS dual-screen layout choice for RetroArch melonDS core opts and
 * standalone melonDS Window0 settings (same Hybrid Top / Top-Bottom policy).
 *
 * Standalone note: melonDS "Hybrid" (layout=3) draws a large pane plus *both*
 * screens as minis, so the small pane is often another copy of the top screen.
 * Landscape instead uses Horizontal + EmphTop (large top left, small bottom right
 * centered at ~⅓ height). SwapScreenEmphasis toggles EmphTop ↔ EmphBot (sizes only;
 * top stays left, bottom stays right).
 */
struct NdsDisplayLayout {
    bool portrait = false;
    /** RetroArch melonds_screen_layout value. */
    const char* core_layout = "Hybrid Top";
    /** melonDS ScreenLayoutType: Horizontal=2, Vertical=1. */
    int window_screen_layout = 2;
    /** melonDS ScreenSizing: EmphTop=1, Even=0. */
    int window_screen_sizing = 1;
};

NdsDisplayLayout resolve_nds_display_layout(
    DisplayLayoutPreference preference = DisplayLayoutPreference::Auto);

} // namespace archstreamer
