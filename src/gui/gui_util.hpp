#pragma once

#include "common/protocol.hpp"
#include "client/client_app.hpp"

#include <QString>

class QComboBox;

namespace archstreamer::gui {

constexpr int DefaultInputPort = 45454;
constexpr int DefaultVideoPort = 5004;
constexpr int DefaultAudioPort = 6004;

QString mode_name(GameSessionMode mode);
GameSessionMode selected_mode(const QComboBox* combo);
ClientParticipantRole selected_client_role(const QComboBox* combo);

} // namespace archstreamer::gui
