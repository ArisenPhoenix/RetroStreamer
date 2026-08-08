#include "host/controls_db_sync.hpp"

#include "common/controls_db_pack.hpp"
#include "host/user_controls_db.hpp"

#include <string>

namespace archstreamer {

ByteBuffer handle_controls_db_packet(
    const std::filesystem::path& save_root,
    std::string_view claimed_username,
    const PacketPayload& payload) {
    // Identity comes from LobbyPresence / session hello — already authenticated.
    // Use that username for the profile path; do not re-validate packet usernames.
    const std::string username(claimed_username);

    if (const auto* pull = std::get_if<ControlsDbPull>(&payload); pull != nullptr) {
        ControlsDbResponse response;
        response.username = username.empty() ? pull->username : username;
        if (save_root.empty() || username.empty()) {
            response.found = false;
            return serialize_packet(response);
        }
        const auto path = user_controls_db_path_for(save_root, username);
        response.db_bytes = read_user_controls_db_file(path);
        response.found = !response.db_bytes.empty();
        return serialize_packet(response);
    }

    if (const auto* push = std::get_if<ControlsDbPush>(&payload); push != nullptr) {
        ControlsDbAck ack;
        ack.username = username.empty() ? push->username : username;
        if (save_root.empty() || username.empty()) {
            ack.ok = false;
            ack.message = "not authenticated";
            return serialize_packet(ack);
        }
        std::string error;
        if (!validate_controls_db_pack(push->db_bytes, username, &error)) {
            ack.ok = false;
            ack.message = error.empty() ? "invalid controls database" : error;
            return serialize_packet(ack);
        }
        // Merge, never replace: a client only pushes the kinds it knows about, and
        // installing its file wholesale would drop the rest of the profile — an
        // overlay_profiles row from a tablet dies the moment a TV pushes a button_map.
        auto rows = import_controls_db_pack(push->db_bytes, username, &error);
        if (!rows.has_value()) {
            ack.ok = false;
            ack.message = error.empty() ? "invalid controls database" : error;
            return serialize_packet(ack);
        }
        if (rows->empty()) {
            ack.ok = false;
            ack.message = "controls database has no rows for " + username;
            return serialize_packet(ack);
        }
        const auto path = user_controls_db_path_for(save_root, username);
        for (const auto& row : *rows) {
            UserControlsRow stored;
            // Store under the authenticated name, not the pack's spelling of it.
            stored.username = username;
            stored.kind = row.kind;
            stored.document_json = row.document_json;
            stored.version = row.version;
            stored.updated_at = row.updated_at;
            if (!upsert_user_controls_row(path, stored)) {
                ack.ok = false;
                ack.message = "failed to store " + row.kind;
                return serialize_packet(ack);
            }
        }
        ack.ok = true;
        ack.message = "ok";
        return serialize_packet(ack);
    }

    return {};
}

} // namespace archstreamer
