#pragma once
#include "common/protocol.hpp"

namespace archstreamer {

class InputSender {
public:
    explicit InputSender(ClientId client_id);
    ControllerInput make_input(LocalPlayerIndex local_player, ControllerState state) const;

private:
    ClientId client_id_;
};

} // namespace archstreamer
