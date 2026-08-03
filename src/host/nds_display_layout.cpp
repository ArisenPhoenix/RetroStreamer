#include "host/nds_display_layout.hpp"

namespace archstreamer {

NdsDisplayLayout resolve_nds_display_layout(DisplayLayoutPreference preference) {
    NdsDisplayLayout layout;
    layout.portrait = preference == DisplayLayoutPreference::Portrait;
    if (layout.portrait) {
        layout.core_layout = "Top/Bottom";
        layout.window_screen_layout = 1; // screenLayout_Vertical
        layout.window_screen_sizing = 0; // screenSizing_Even
    } else {
        layout.core_layout = "Hybrid Top";
        // Not screenLayout_Hybrid (3): that draws large+mini-top+mini-bot, so the
        // "small" screen is often another top. Horizontal+EmphTop = large top,
        // small bottom — same idea as libretro Hybrid Top / small_screen=Bottom.
        layout.window_screen_layout = 2; // screenLayout_Horizontal
        layout.window_screen_sizing = 1; // screenSizing_EmphTop
    }
    return layout;
}

} // namespace archstreamer
