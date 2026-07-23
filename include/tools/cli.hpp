#pragma once

#include "client/client_app.hpp"
#include "host/host_app_config.hpp"

#include <functional>
#include <iosfwd>
#include <optional>
#include <string>

namespace archstreamer {

using HostRunnerCliArgs = HostAppConfig;

struct SessionClientCliArgs {
    std::string host = "127.0.0.1";
    std::uint16_t port = 45555;
    std::optional<std::uint16_t> input_port;
    std::string username;
    std::string display_name;
    ParticipantRole role = ParticipantRole::Player;
    GameFilterMode filter_mode = GameFilterMode::Any;
    GameSessionMode session_mode = GameSessionMode::SinglePlayer;
    bool session_mode_explicit = false;
    std::uint8_t players = 1;
    std::optional<std::string> game_selector;
    std::optional<std::string> system_filter;
    std::optional<std::string> language_filter;
    std::vector<std::size_t> controller_indexes;
    bool list_games = false;
    bool list_systems = false;
    bool list_languages = false;
    bool active_session = false;
    bool wants_video = true;
    bool wants_audio = true;
    bool synced_av = false;
    MediaQualityTier wanted_tier = MediaQualityTier::Auto;

    ClientAppConfig app_config() const;
};

class HostRunnerCli {
public:
    explicit HostRunnerCli(std::ostream& out, std::ostream& err);

    HostAppConfig parse(int argc, char** argv) const;
    void print_usage() const;

private:
    std::ostream& out_;
    std::ostream& err_;
};

class SessionClientCli {
public:
    explicit SessionClientCli(std::ostream& out, std::ostream& err);

    SessionClientCliArgs parse(int argc, char** argv) const;
    int run(const SessionClientCliArgs& args, const ClientApp& app, const std::function<bool()>& should_stop) const;
    void print_usage() const;

private:
    std::ostream& out_;
    std::ostream& err_;
};

} // namespace archstreamer
