#include "profile_controls.hpp"

namespace archstreamer::gui {

std::optional<ControllerMapDocument> load_controller_map_from_profile(
    const std::filesystem::path&,
    const std::string&) {
    return std::nullopt;
}

bool upsert_controller_map_to_profile(
    const std::filesystem::path&,
    const std::string&,
    const ControllerMapDocument&) {
    return false;
}

} // namespace archstreamer::gui
