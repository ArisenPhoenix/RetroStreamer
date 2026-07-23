#include "tools/cli.hpp"

#include "common/cli_common.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string_view>

namespace archstreamer {
namespace {

GameFilterMode parse_filter_mode(std::string_view value) {
    if (value == "any" || value == "all") {
        return GameFilterMode::Any;
    }
    if (value == "singleplayer" || value == "single" || value == "sp") {
        return GameFilterMode::SinglePlayer;
    }
    if (value == "multiplayer" || value == "multi" || value == "mp") {
        return GameFilterMode::Multiplayer;
    }

    throw std::runtime_error("filter mode must be any, single, or multi");
}

void print_games(std::ostream& out, const GameList& list) {
    for (std::size_t i = 0; i < list.games.size(); ++i) {
        const auto& game = list.games[i];
        out
            << i << ": " << game.display_name
            << " | " << game.system_name
            << " [" << game.system_key << ']'
            << " | " << game.core_name
            << " | " << game.language
            << '/' << game.region
            << '/' << game.version
            << " | players " << static_cast<int>(game.min_players)
            << '-' << static_cast<int>(game.max_players)
            << " | modes"
            << (game.supports_singleplayer ? " single" : "")
            << (game.supports_multiplayer ? " multi" : "")
            << "\n   id=" << game.id
            << "\n   asset_key=" << game.asset_key
            << '\n';
    }
}

std::string mode_name(GameSessionMode mode) {
    return mode == GameSessionMode::SinglePlayer ? "singleplayer" : "multiplayer";
}

void print_active_session(std::ostream& out, const ActiveSessionInfo& info) {
    if (!info.active) {
        out << "No active session.\n";
        return;
    }

    out
        << "Active session:"
        << "\n  game: " << info.selected_game_id.value_or("")
        << "\n  mode: " << mode_name(info.session_mode)
        << "\n  players: " << static_cast<int>(info.player_count)
        << "\n  connected_players: " << static_cast<int>(info.connected_players)
        << "\n  disconnected_players: " << static_cast<int>(info.disconnected_players)
        << "\n  viewers: " << static_cast<int>(info.viewer_count)
        << "\n  video: " << (info.video_enabled ? "yes" : "no")
        << "\n  audio: " << (info.audio_enabled ? "yes" : "no")
        << '\n';
}

} // namespace

ClientAppConfig SessionClientCliArgs::app_config() const {
    ClientAppConfig config;
    config.host = host;
    config.control_port = port;
    config.input_port = input_port;
    config.username = username;
    config.display_name = display_name;
    config.role = role;
    config.filter = GameFilter{
        filter_mode,
        players,
        system_filter,
        language_filter,
    };
    config.session_mode = session_mode;
    config.game_selector = game_selector;
    config.controller_indexes = controller_indexes;
    config.wants_video = wants_video;
    config.wants_audio = wants_audio;
    config.synced_av = synced_av;
    config.wanted_tier = wanted_tier;
    return config;
}

SessionClientCli::SessionClientCli(std::ostream& out, std::ostream& err)
    : out_(out), err_(err) {
}

void SessionClientCli::print_usage() const {
    out_
        << "usage: session_client [--host 127.0.0.1] [--port 45555] [--username name]\n"
        << "                      [--display-name name] [--role player|viewer] [--mode singleplayer|multiplayer]\n"
        << "                      [--players 0|1|2]\n"
        << "                      [--filter-mode any|single|multi] [--system name] [--list-systems]\n"
        << "                      [--language code] [--list-languages]\n"
        << "                      [--active-session]\n"
        << "                      [--controller index] [--controller index] [--game index-or-id]\n"
        << "                      [--input-port port] [--no-video] [--no-audio] [--synced-av]\n"
        << "                      [--stream-quality auto|low|medium|high] [--list-games]\n";
}

SessionClientCliArgs SessionClientCli::parse(int argc, char** argv) const {
    SessionClientCliArgs args;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--host") {
            if (++i >= argc) {
                throw std::runtime_error("--host requires an IPv4 address");
            }
            args.host = argv[i];
        } else if (arg == "--port") {
            if (++i >= argc) {
                throw std::runtime_error("--port requires a value");
            }
            args.port = static_cast<std::uint16_t>(std::stoul(argv[i]));
        } else if (arg == "--input-port") {
            if (++i >= argc) {
                throw std::runtime_error("--input-port requires a value");
            }
            args.input_port = static_cast<std::uint16_t>(std::stoul(argv[i]));
        } else if (arg == "--username") {
            if (++i >= argc) {
                throw std::runtime_error("--username requires a value");
            }
            args.username = argv[i];
        } else if (arg == "--display-name") {
            if (++i >= argc) {
                throw std::runtime_error("--display-name requires a value");
            }
            args.display_name = argv[i];
        } else if (arg == "--role") {
            if (++i >= argc) {
                throw std::runtime_error("--role requires player or viewer");
            }
            args.role = parse_participant_role(argv[i]);
        } else if (arg == "--mode") {
            if (++i >= argc) {
                throw std::runtime_error("--mode requires singleplayer or multiplayer");
            }
            args.session_mode = parse_session_mode(argv[i]);
            args.session_mode_explicit = true;
        } else if (arg == "--filter-mode") {
            if (++i >= argc) {
                throw std::runtime_error("--filter-mode requires any, single, or multi");
            }
            args.filter_mode = parse_filter_mode(argv[i]);
        } else if (arg == "--system") {
            if (++i >= argc) {
                throw std::runtime_error("--system requires a system name");
            }
            args.system_filter = argv[i];
        } else if (arg == "--language") {
            if (++i >= argc) {
                throw std::runtime_error("--language requires a language code");
            }
            args.language_filter = argv[i];
        } else if (arg == "--list-systems") {
            args.list_systems = true;
        } else if (arg == "--list-languages") {
            args.list_languages = true;
        } else if (arg == "--active-session") {
            args.active_session = true;
        } else if (arg == "--players") {
            if (++i >= argc) {
                throw std::runtime_error("--players requires a value");
            }
            args.players = static_cast<std::uint8_t>(std::stoul(argv[i]));
        } else if (arg == "--controller") {
            if (++i >= argc) {
                throw std::runtime_error("--controller requires an index");
            }
            args.controller_indexes.push_back(static_cast<std::size_t>(std::stoul(argv[i])));
        } else if (arg == "--game") {
            if (++i >= argc) {
                throw std::runtime_error("--game requires an index or id");
            }
            args.game_selector = argv[i];
        } else if (arg == "--list-games") {
            args.list_games = true;
        } else if (arg == "--no-video") {
            args.wants_video = false;
        } else if (arg == "--no-audio") {
            args.wants_audio = false;
        } else if (arg == "--synced-av") {
            args.synced_av = true;
        } else if (arg == "--stream-quality") {
            if (++i >= argc) {
                throw std::runtime_error("--stream-quality requires auto|low|medium|high");
            }
            const std::string value = argv[i];
            if (value == "auto") {
                args.wanted_tier = MediaQualityTier::Auto;
            } else if (value == "low") {
                args.wanted_tier = MediaQualityTier::Low;
            } else if (value == "medium") {
                args.wanted_tier = MediaQualityTier::Medium;
            } else if (value == "high") {
                args.wanted_tier = MediaQualityTier::High;
            } else {
                throw std::runtime_error("--stream-quality requires auto|low|medium|high");
            }
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    if (args.username.empty()) {
        args.username = default_cli_username();
    }
    if (args.display_name.empty()) {
        args.display_name = args.username;
    }
    if (!valid_player_count(args.players)) {
        throw std::runtime_error("--players must be 0, 1, or 2");
    }
    if (args.role == ParticipantRole::Viewer) {
        args.players = 0;
        args.controller_indexes.clear();
    }
    if (!args.session_mode_explicit) {
        args.session_mode = default_session_mode_for_filter(args.filter_mode);
    }
    if (args.controller_indexes.empty()) {
        for (std::uint8_t i = 0; i < args.players; ++i) {
            args.controller_indexes.push_back(i);
        }
    }
    if (args.controller_indexes.size() < args.players) {
        throw std::runtime_error("not enough --controller values for requested players");
    }
    if (args.controller_indexes.size() > MaxPlayersPerClient) {
        throw std::runtime_error("client can select at most two controllers");
    }

    return args;
}

int SessionClientCli::run(
    const SessionClientCliArgs& args,
    const ClientApp& app,
    const std::function<bool()>& should_stop) const {
    if (args.active_session) {
        print_active_session(out_, app.active_session_info(args.host, args.port));
        return 0;
    }

    if (args.list_games || args.list_systems || args.list_languages) {
        const auto catalog = app.fetch_catalog(args.app_config());
        out_ << "Received " << catalog.full_catalog.games.size() << " games from host.\n";
        if (args.list_systems) {
            for (const auto& system : systems_for_games(catalog.full_catalog)) {
                out_ << system << '\n';
            }
        }
        if (args.list_languages) {
            for (const auto& language : languages_for_games(catalog.full_catalog)) {
                out_ << language << '\n';
            }
        }
        if (args.list_games) {
            out_ << "Showing " << catalog.filtered_catalog.games.size() << " game(s) after filters.\n";
            print_games(out_, catalog.filtered_catalog);
        }
        return 0;
    }

    auto connected_client_id = std::optional<ClientId>{};
    ClientAppCallbacks callbacks;
    callbacks.on_catalog = [&](const GameList& game_list, const GameList& filtered_games) {
        out_ << "Received " << game_list.games.size() << " games from host.\n";
        if (args.list_systems) {
            for (const auto& system : systems_for_games(game_list)) {
                out_ << system << '\n';
            }
        }
        if (args.list_languages) {
            for (const auto& language : languages_for_games(game_list)) {
                out_ << language << '\n';
            }
        }
        if (args.list_games) {
            out_ << "Showing " << filtered_games.games.size() << " game(s) after filters.\n";
            print_games(out_, filtered_games);
        }
    };
    callbacks.on_connected = [&](const ClientConnectionInfo& connection) {
        connected_client_id = connection.client_id;
        out_
            << "Connected as client " << static_cast<int>(connection.client_id)
            << " username=" << connection.username
            << " role=" << participant_role_name(connection.role)
            << " mode=" << mode_name(connection.session_mode);
        if (connection.selected_game_id.has_value()) {
            out_ << " game=\"" << *connection.selected_game_id << "\"";
        }
        out_ << '\n';
    };
    callbacks.on_seat_assignment = [&](const SeatAssignment& seats) {
        if (seats.seats.empty()) {
            out_ << "Assigned as viewer only.\n";
            return;
        }
        for (const auto& seat : seats.seats) {
            if (!connected_client_id.has_value() || seat.client_id != *connected_client_id) {
                continue;
            }
            out_
                << "Local P" << static_cast<int>(seat.local_player) + 1
                << " -> RetroArch P" << static_cast<int>(seat.retroarch_port) + 1
                << '\n';
        }
    };
    callbacks.on_session_ready = [&](const SessionReady& ready) {
        out_ << "Session ready: " << static_cast<int>(ready.player_count) << " player(s).\n";
    };
    callbacks.on_media_endpoint = [&](const MediaEndpoint& endpoint) {
        if (args.wants_video && !endpoint.video_uri.empty()) {
            out_ << "Receiving video from " << endpoint.video_uri << ".\n";
        }
        if (args.wants_audio && !endpoint.audio_uri.empty()) {
            out_ << "Receiving audio from " << endpoint.audio_uri << ".\n";
        }
    };
    callbacks.on_session_starting = [&](const SessionStarting& starting) {
        out_ << "Session starting: " << static_cast<int>(starting.player_count) << " player(s).\n";
    };
    callbacks.on_session_ended = [&](const std::string& reason) {
        out_ << "Session ended: " << reason << '\n';
    };
    callbacks.on_host_disconnected = [&] {
        out_ << "Host control connection closed; stopping input stream.\n";
    };
    callbacks.on_input_streaming_started = [&](const std::string& host, std::uint16_t input_port) {
        out_
            << "Streaming controller input to " << host << ':' << input_port
            << ". Press Ctrl+C to stop.\n";
    };
    callbacks.on_waiting_without_input = [&] {
        out_ << "Waiting for session end. Press Ctrl+C to stop.\n";
    };

    app.run_session(args.app_config(), should_stop, callbacks);
    return 0;
}

} // namespace archstreamer
