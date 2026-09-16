#pragma once

#include "common/catalog_paths.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace archstreamer {

/**
 * Catalog/game knobs. Console owns this; HostAppConfig embeds it so session
 * resolves ROM/meta/art paths from here instead of loose HostAppConfig fields.
 */
struct GameConfig {
    std::filesystem::path rom_root = DefaultRomRoot;
    std::filesystem::path meta_root;
    std::filesystem::path art_root;
    std::optional<std::string> selector;
};

} // namespace archstreamer
