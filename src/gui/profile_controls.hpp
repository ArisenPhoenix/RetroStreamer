#pragma once

#include "common/controller_button_map.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace archstreamer::gui {

/**
 * Per-user controller maps kept in the save profile's controls database.
 *
 * A client-only build has no profile database: the load yields nullopt and the
 * upsert reports false, so callers fall through to the cadence store and then to
 * the AppConfig JSON exactly as a host build does when the profile is missing.
 */
std::optional<ControllerMapDocument> load_controller_map_from_profile(
    const std::filesystem::path& save_root,
    const std::string& username);

bool upsert_controller_map_to_profile(
    const std::filesystem::path& save_root,
    const std::string& username,
    const ControllerMapDocument& document);

} // namespace archstreamer::gui
