#pragma once

#include "common/protocol.hpp"

#include <iosfwd>
#include <string>

namespace archstreamer {

std::string format_game_summary(const GameInfo& game);
void print_game_catalog(std::ostream& out, const GameList& list);

const GameInfo* find_game_by_id(const GameList& list, const GameId& game_id);

} // namespace archstreamer
