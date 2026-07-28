#pragma once

#include "host/host_app_config.hpp"
#include "host/streaming_audio_sink.hpp"
#include "client/controller_manager.hpp"
#include "common/protocol.hpp"

#include <functional>
#include <optional>

namespace archstreamer {

class GameCatalog;
struct GameList;
struct HostPlayerControllerIdentity;

class HostApp {
public:
    explicit HostApp(HostAppConfig config);

    int run(const std::function<bool()>& should_stop);

private:
    /** Disable stream A/V when Host role is local Player. */
    static void apply_host_player_media_policy(HostAppConfig& config, bool host_plays_locally);

    /** Resolve/create Pulse null-sink monitor source when streaming audio. */
    static void prepare_streaming_audio_source(HostAppConfig& config, StreamingAudioSink& sink);

    /** Validate host role + optional bridge controller; returns bridge device if Player. */
    static std::optional<ControllerDevice> resolve_bridge_device(const HostAppConfig& config);

    int run_lobby_sessions(
        HostAppConfig config,
        GameCatalog& catalog,
        const GameList& list,
        StreamingAudioSink& streaming_audio,
        std::optional<ControllerDevice> bridge_device,
        const std::function<bool()>& should_stop);

    int run_direct_session(
        HostAppConfig config,
        GameCatalog& catalog,
        const GameList& list,
        StreamingAudioSink& streaming_audio,
        std::optional<ControllerDevice> bridge_device,
        std::optional<HostPlayerControllerIdentity> bridge_identity,
        bool host_plays_locally,
        const std::function<bool()>& should_stop);

    HostAppConfig config_;
};

} // namespace archstreamer
