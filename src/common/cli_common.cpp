#include "common/cli_common.hpp"

#include "common/platform/paths.hpp"
#include "common/protocol.hpp"

#include <stdexcept>

namespace archstreamer {

std::string default_cli_username() {
    const auto user = current_username();
    if (valid_username(user)) {
        return user;
    }
    return "local";
}

GameSessionMode parse_session_mode(std::string_view value) {
    if (value == "singleplayer" || value == "single" || value == "solo" || value == "sp") {
        return GameSessionMode::SinglePlayer;
    }
    if (value == "multiplayer" || value == "multi" || value == "coop" || value == "co-op" || value == "mp") {
        return GameSessionMode::Multiplayer;
    }

    throw std::runtime_error("mode must be singleplayer or multiplayer");
}

} // namespace archstreamer
