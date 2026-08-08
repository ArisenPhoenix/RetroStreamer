#include "gui_util.hpp"

#include <QComboBox>

#include <filesystem>

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

bool running_inside_flatpak() {
    if (qEnvironmentVariableIsSet("FLATPAK_ID")) {
        return true;
    }
    return std::filesystem::exists("/.flatpak-info");
}

} // namespace archstreamer::gui
