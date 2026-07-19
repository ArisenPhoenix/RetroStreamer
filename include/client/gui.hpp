#pragma once

#include "client/controller_manager.hpp"
#include "common/protocol.hpp"

#include <optional>
#include <vector>

namespace archstreamer {

struct ControllerPickerRow {
    LocalPlayerIndex local_player = 0;
    std::string controller_name;
    std::optional<RetroArchPort> assigned_port;
};

struct ClientGuiState {
    ClientRole role = ClientRole::Viewer;
    std::vector<GameInfo> available_games;
    std::optional<GameId> selected_game_id;
    std::vector<ControllerPickerRow> player_rows;
};

ClientGuiState build_gui_state(
    const ControllerManager& controllers,
    const GameList& game_list,
    std::optional<GameId> selected_game_id,
    const SeatAssignment& seats,
    ClientId client_id);

} // namespace archstreamer
