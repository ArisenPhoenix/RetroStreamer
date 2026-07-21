#pragma once

#include "common/protocol.hpp"

#include <string>
#include <string_view>

namespace archstreamer {

std::string default_cli_username();
GameSessionMode parse_session_mode(std::string_view value);

} // namespace archstreamer
