#include "client/gui.hpp"

#include <utility>

namespace archstreamer {

ClientGuiState build_gui_state(
    const ControllerManager& controllers,
    const GameList& game_list,
    std::optional<GameId> selected_game_id,
    const SeatAssignment& seats,
    ClientId client_id) {
    ClientGuiState state;
    state.role = role_for_player_count(controllers.requested_player_count());
    state.available_games = game_list.games;
    state.selected_game_id = std::move(selected_game_id);

    for (const auto& selected : controllers.selected_controllers()) {
        std::optional<RetroArchPort> assigned_port;
        for (const auto& seat : seats.seats) {
            if (seat.client_id == client_id && seat.local_player == selected.local_player) {
                assigned_port = seat.retroarch_port;
                break;
            }
        }

        state.player_rows.push_back(ControllerPickerRow{
            selected.local_player,
            selected.device.name,
            assigned_port,
        });
    }

    return state;
}

} // namespace archstreamer
