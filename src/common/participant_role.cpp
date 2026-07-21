#include "common/participant_role.hpp"

#include <stdexcept>

namespace archstreamer {

const char* participant_role_name(ParticipantRole role) {
    return role == ParticipantRole::Player ? "player" : "viewer";
}

ParticipantRole parse_participant_role(std::string_view value) {
    if (value == "player" || value == "play") {
        return ParticipantRole::Player;
    }
    if (value == "viewer" || value == "view" || value == "spectator" || value == "spectate") {
        return ParticipantRole::Viewer;
    }

    throw std::runtime_error("role must be player or viewer");
}

} // namespace archstreamer
