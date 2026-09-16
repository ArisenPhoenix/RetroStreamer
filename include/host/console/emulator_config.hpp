#pragma once

namespace archstreamer {

/** Internal-resolution multipliers for Switch standalone and RetroArch cores. */
struct ResolutionSettings {
    // Switch standalone IR (Yuzu qt-config + Ryujinx res_scale). Clamp 1–6 at write;
    // Ryujinx caps at 4 internally.
    int switch_scale = 1;
    // RetroArch IR via known cores' .opt files (1–6).
    int retroarch_scale = 1;
};

/**
 * Emulator-facing knobs that are not a specific backend profile.
 * Console owns this; HostAppConfig embeds it.
 */
struct EmulatorConfig {
    ResolutionSettings resolution;
};

} // namespace archstreamer
