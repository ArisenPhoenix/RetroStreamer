#pragma once

#include "host/game_catalog.hpp"
#include "host/host_app_config.hpp"
#include "host/streaming_audio_sink.hpp"
#include "client/controller_manager.hpp"

#include <functional>
#include <optional>

namespace archstreamer {

/** Persistent control listener: concurrent SP slots + one Multi lobby at a time. */
int run_concurrent_session_host(
    HostAppConfig config,
    GameCatalog& catalog,
    const GameList& list,
    StreamingAudioSink& streaming_audio,
    std::optional<ControllerDevice> bridge_device,
    const std::function<bool()>& should_stop);

} // namespace archstreamer
