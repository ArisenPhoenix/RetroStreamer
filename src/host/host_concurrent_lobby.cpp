#include "host/host_concurrent_lobby.hpp"

#include "host/lobby.hpp"

#include <utility>

namespace archstreamer {

int run_concurrent_session_host(
    HostAppConfig config,
    GameCatalog& catalog,
    const GameList& list,
    StreamingAudioSink& streaming_audio,
    std::optional<ControllerDevice> bridge_device,
    const std::function<bool()>& should_stop) {
    Lobby::Config lobby_config;
    lobby_config.host_config = std::move(config);
    lobby_config.catalog = &catalog;
    lobby_config.game_list = list;
    lobby_config.streaming_audio = &streaming_audio;
    lobby_config.bridge_device = std::move(bridge_device);
    lobby_config.should_stop = should_stop;

    Lobby lobby(std::move(lobby_config));
    // Lobby accept thread + HostApp-style apply loop (commands → SessionManager).
    return lobby.run_until_stop();
}

} // namespace archstreamer
