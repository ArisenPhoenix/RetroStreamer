#pragma once

namespace archstreamer {

/** Relative layout under a user-chosen Gaming root (no absolute machine path). */
inline constexpr const char* RelRomRoot = "ROMS/Games";
inline constexpr const char* RelMetaRoot = "ROMS/Meta";
inline constexpr const char* RelArtRoot = "ROMS/Art";
/** Global updates/DLC packs + unpacked Switch NCAs: ROMS/DLC/<System>/<game_id>/. */
inline constexpr const char* RelDlcRoot = "ROMS/DLC";
inline constexpr const char* RelBiosRoot = "BIOS FILES";
inline constexpr const char* RelSrmDir = "tools/srm";

/**
 * Absolute catalog defaults are empty — set via Settings, CLI (--rom-root / --art-root /
 * --meta-root), or by joining a Gaming root with Rel* above.
 */
inline constexpr const char* DefaultRomRoot = "";
inline constexpr const char* DefaultMetaRoot = "";
inline constexpr const char* DefaultArtRoot = "";

} // namespace archstreamer
