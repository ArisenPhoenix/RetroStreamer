#include "profile_controls.hpp"

#include "host/user_controls_db.hpp"

namespace archstreamer::gui {

std::optional<ControllerMapDocument> load_controller_map_from_profile(
    const std::filesystem::path& save_root,
    const std::string& username) {
    if (save_root.empty() || username.empty()) {
        return std::nullopt;
    }
    auto found = find_user_controls_row(
        user_controls_db_path_for(save_root, username),
        username,
        kControlsKindButtonMap);
    if (!found.has_value()) {
        return std::nullopt;
    }
    return controller_map_document_from_json(found->document_json);
}

bool upsert_controller_map_to_profile(
    const std::filesystem::path& save_root,
    const std::string& username,
    const ControllerMapDocument& document) {
    if (save_root.empty() || username.empty()) {
        return false;
    }
    UserControlsRow row;
    row.username = username;
    row.kind = std::string(kControlsKindButtonMap);
    row.document_json = controller_map_document_to_json(document);
    row.version = ControllerMapDocumentVersion;
    return upsert_user_controls_row(user_controls_db_path_for(save_root, username), row);
}

} // namespace archstreamer::gui
