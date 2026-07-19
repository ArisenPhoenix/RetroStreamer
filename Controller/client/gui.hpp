#pragma once

#include "controller_manager.hpp"
#include "../common/protocol.hpp"

#include <optional>
#include <string>
#include <vector>

namespace archstreamer {

struct ControllerPickerRow {
    LocalPlayerIndex local_player = 0;
    std::string controller_name;
    std::optional<RetroArchPort> assigned_port;
};

struct ClientGuiState {
    ClientRole role = ClientRole::Viewer;
    std::vector<ControllerPickerRow> player_rows;
};

inline ClientGuiState build_gui_state(
    const ControllerManager& controllers,
    const SeatAssignment& seats,
    ClientId client_id) {
    ClientGuiState state;
    state.role = role_for_player_count(controllers.requested_player_count());

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
