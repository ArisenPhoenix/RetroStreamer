#pragma once

#include <string_view>

namespace archstreamer {

enum class ParticipantRole {
    Player,
    Viewer,
};

const char* participant_role_name(ParticipantRole role);
ParticipantRole parse_participant_role(std::string_view value);

} // namespace archstreamer
