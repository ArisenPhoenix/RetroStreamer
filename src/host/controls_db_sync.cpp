#include "host/controls_db_sync.hpp"

#include "host/user_controls_db.hpp"

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
        const auto path = user_controls_db_path_for(save_root, username);
        if (!write_user_controls_db_file(path, push->db_bytes, &error)) {
            ack.ok = false;
            ack.message = error.empty() ? "failed to write controls database" : error;
            return serialize_packet(ack);
        }
        ack.ok = true;
        ack.message = "ok";
        return serialize_packet(ack);
    }

    return {};
}

} // namespace archstreamer
