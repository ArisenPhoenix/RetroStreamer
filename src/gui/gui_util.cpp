#include "gui_util.hpp"

#include <QComboBox>

namespace archstreamer::gui {

QString mode_name(GameSessionMode mode) {
    return mode == GameSessionMode::SinglePlayer ? "singleplayer" : "multiplayer";
}

GameSessionMode selected_mode(const QComboBox* combo) {
    return combo->currentIndex() == 1
        ? GameSessionMode::Multiplayer
        : GameSessionMode::SinglePlayer;
}

ClientParticipantRole selected_client_role(const QComboBox* combo) {
    return combo->currentIndex() == 1
        ? ClientParticipantRole::Viewer
        : ClientParticipantRole::Player;
}

} // namespace archstreamer::gui
