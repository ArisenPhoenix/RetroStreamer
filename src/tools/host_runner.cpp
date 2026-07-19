#include "client/controller_backend.hpp"
#include "common/serialization.hpp"
#include "common/udp_socket.hpp"
#include "host/game_catalog_scanner.hpp"
#include "host/gstreamer_media_server.hpp"
#include "host/input_router.hpp"
#include "host/linux_uinput_gamepad.hpp"
#include "host/posix_retroarch_process.hpp"
#include "host/save_profile.hpp"
#include "host/session_service.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr const char* DefaultRomRoot = "/mnt/Internal_SSD/Gaming/ROMS/Games";

std::atomic_bool stop_requested = false;

void handle_signal(int) {
    stop_requested = true;
}

enum class ParticipantRole {
    Player,
    Viewer,
};

struct Args {
    std::filesystem::path rom_root = DefaultRomRoot;
    std::filesystem::path meta_root;
    std::optional<std::string> selector;
    archstreamer::GameSessionMode session_mode = archstreamer::GameSessionMode::SinglePlayer;
    std::uint8_t players = 1;
    bool list = false;
    bool dry_run = false;
    bool pulse_input = false;
    bool verbose = false;
    std::optional<std::uint16_t> control_port;
    std::optional<std::uint16_t> input_port;
    std::uint8_t clients = 1;
    std::uint16_t session_timeout_seconds = 30;
    std::uint16_t client_timeout_seconds = 5;
    std::uint16_t player_reconnect_timeout_seconds = 60;
    ParticipantRole host_role = ParticipantRole::Player;
    bool video = false;
    std::string video_destination = "127.0.0.1";
    bool video_destination_explicit = false;
    std::uint16_t video_port = 5004;
    bool audio = false;
    std::uint16_t audio_port = 6004;
    std::string audio_source;
    archstreamer::AudioCaptureBackend audio_backend = archstreamer::AudioCaptureBackend::Pulse;
    std::string virtual_display = ":99";
    std::string video_resolution = "1280x720";
    archstreamer::VirtualDisplayBackend display_backend = archstreamer::VirtualDisplayBackend::None;
    std::optional<std::size_t> bridge_controller_index;
    std::optional<std::size_t> virtual_joypad_index;
    std::optional<std::string> ignore_controller;
    std::string retroarch_joypad_driver = "udev";
    std::filesystem::path save_root = archstreamer::default_save_profile_root();
    std::string username;
};

bool is_number(std::string_view value) {
    if (value.empty()) {
        return false;
    }

    for (const auto character : value) {
        if (character < '0' || character > '9') {
            return false;
        }
    }

    return true;
}

archstreamer::GameSessionMode parse_session_mode(std::string_view value) {
    if (value == "singleplayer" || value == "single" || value == "solo") {
        return archstreamer::GameSessionMode::SinglePlayer;
    }
    if (value == "multiplayer" || value == "multi" || value == "coop" || value == "co-op") {
        return archstreamer::GameSessionMode::Multiplayer;
    }

    throw std::runtime_error("--mode must be singleplayer or multiplayer");
}

ParticipantRole parse_role(std::string_view value) {
    if (value == "player" || value == "play") {
        return ParticipantRole::Player;
    }
    if (value == "viewer" || value == "view" || value == "spectator" || value == "spectate") {
        return ParticipantRole::Viewer;
    }

    throw std::runtime_error("role must be player or viewer");
}

archstreamer::VirtualDisplayBackend parse_display_backend(std::string_view value) {
    if (value == "auto") {
        return archstreamer::VirtualDisplayBackend::None;
    }
    if (value == "xvfb") {
        return archstreamer::VirtualDisplayBackend::Xvfb;
    }
    if (value == "xephyr") {
        return archstreamer::VirtualDisplayBackend::Xephyr;
    }

    throw std::runtime_error("--display-backend must be auto, xvfb, or xephyr");
}

archstreamer::AudioCaptureBackend parse_audio_backend(std::string_view value) {
    if (value == "pulse" || value == "pulseaudio") {
        return archstreamer::AudioCaptureBackend::Pulse;
    }
    if (value == "pipewire" || value == "pw") {
        return archstreamer::AudioCaptureBackend::PipeWire;
    }

    throw std::runtime_error("--audio-backend must be pulse or pipewire");
}

std::string trim_ascii_whitespace(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    std::size_t start = 0;
    while (start < value.size() && (value[start] == '\n' || value[start] == '\r' || value[start] == ' ' || value[start] == '\t')) {
        ++start;
    }
    if (start > 0) {
        value.erase(0, start);
    }
    return value;
}

std::string read_command_output(const char* command) {
    auto* pipe = popen(command, "r");
    if (pipe == nullptr) {
        return "";
    }

    std::string output;
    std::array<char, 256> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    pclose(pipe);
    return trim_ascii_whitespace(std::move(output));
}

std::string default_pulse_monitor_source() {
    const auto sink = read_command_output("pactl get-default-sink 2>/dev/null");
    if (sink.empty()) {
        return "";
    }

    return sink + ".monitor";
}

const char* role_name(ParticipantRole role) {
    return role == ParticipantRole::Player ? "player" : "viewer";
}

std::string video_destination_for_client(const Args& args, const archstreamer::SessionClientConnection& client) {
    if (args.video_destination_explicit) {
        return args.video_destination;
    }
    if (!client.stream.peer_address().empty()) {
        return client.stream.peer_address();
    }

    return args.video_destination;
}

std::vector<std::pair<archstreamer::ClientId, std::string>> media_destinations_for_session(
    const Args& args,
    const archstreamer::SessionPlan& plan) {
    auto destinations = std::vector<std::pair<archstreamer::ClientId, std::string>>{};
    for (const auto& client : plan.clients) {
        if ((args.video && client.hello.wants_video) || (args.audio && client.hello.wants_audio)) {
            destinations.emplace_back(client.client_id, video_destination_for_client(args, client));
        }
    }
    return destinations;
}

std::vector<archstreamer::GStreamerRtpStreamRequest> video_requests_from_media_destinations(
    const Args& args,
    const std::vector<std::pair<archstreamer::ClientId, std::string>>& destinations) {
    auto requests = std::vector<archstreamer::GStreamerRtpStreamRequest>{};
    requests.reserve(destinations.size());

    for (std::size_t index = 0; index < destinations.size(); ++index) {
        const auto& [client_id, host] = destinations[index];
        requests.push_back(archstreamer::GStreamerRtpStreamRequest{
            client_id,
            host,
            static_cast<std::uint16_t>(args.video_port + index),
        });
    }

    return requests;
}

std::vector<archstreamer::GStreamerRtpStreamRequest> audio_requests_from_media_destinations(
    const Args& args,
    const std::vector<std::pair<archstreamer::ClientId, std::string>>& destinations) {
    auto requests = std::vector<archstreamer::GStreamerRtpStreamRequest>{};
    requests.reserve(destinations.size());

    for (std::size_t index = 0; index < destinations.size(); ++index) {
        const auto& [client_id, host] = destinations[index];
        requests.push_back(archstreamer::GStreamerRtpStreamRequest{
            client_id,
            host,
            static_cast<std::uint16_t>(args.audio_port + index),
        });
    }

    return requests;
}

std::vector<archstreamer::GStreamerMediaStream> media_streams_for_dry_run(
    const Args& args,
    const std::vector<std::pair<archstreamer::ClientId, std::string>>& destinations) {
    auto streams = std::vector<archstreamer::GStreamerMediaStream>{};
    streams.reserve(destinations.size());

    archstreamer::GStreamerVideoSender endpoint_builder;
    archstreamer::GStreamerAudioSender audio_endpoint_builder;
    for (std::size_t index = 0; index < destinations.size(); ++index) {
        const auto& [client_id, host] = destinations[index];
        auto endpoint = archstreamer::MediaEndpoint{};
        if (args.video) {
            endpoint.video_uri = endpoint_builder.endpoint(host, static_cast<std::uint16_t>(args.video_port + index)).video_uri;
        }
        if (args.audio) {
            endpoint.audio_uri = audio_endpoint_builder.endpoint(host, static_cast<std::uint16_t>(args.audio_port + index)).audio_uri;
        }
        streams.push_back(archstreamer::GStreamerMediaStream{
            client_id,
            host,
            endpoint,
        });
    }

    return streams;
}

class ActiveMediaStreams {
public:
    ActiveMediaStreams(
        const Args& args,
        std::optional<archstreamer::GStreamerVideoFanout>& video_fanout,
        std::optional<archstreamer::GStreamerAudioFanout>& audio_fanout)
        : args_(args), video_fanout_(video_fanout), audio_fanout_(audio_fanout) {
    }

    archstreamer::MediaEndpoint start_for_client(
        archstreamer::ClientId client_id,
        const std::string& destination_host,
        std::size_t media_index) {
        auto endpoint = archstreamer::MediaEndpoint{};
        if (args_.video && video_fanout_.has_value()) {
            const auto stream = video_fanout_->add(
                args_.virtual_display,
                archstreamer::GStreamerRtpStreamRequest{
                    client_id,
                    destination_host,
                    static_cast<std::uint16_t>(args_.video_port + media_index),
                });
            endpoint.video_uri = stream.endpoint.video_uri;
        }
        if (args_.audio && audio_fanout_.has_value()) {
            const auto stream = audio_fanout_->add(
                args_.audio_backend,
                args_.audio_source,
                archstreamer::GStreamerRtpStreamRequest{
                    client_id,
                    destination_host,
                    static_cast<std::uint16_t>(args_.audio_port + media_index),
                });
            endpoint.audio_uri = stream.endpoint.audio_uri;
        }

        return endpoint;
    }

    void stop_for_client(archstreamer::ClientId client_id) {
        if (video_fanout_.has_value()) {
            video_fanout_->stop_client(client_id);
        }
        if (audio_fanout_.has_value()) {
            audio_fanout_->stop_client(client_id);
        }
    }

private:
    const Args& args_;
    std::optional<archstreamer::GStreamerVideoFanout>& video_fanout_;
    std::optional<archstreamer::GStreamerAudioFanout>& audio_fanout_;
};

archstreamer::ClientId next_session_client_id(const archstreamer::SessionPlan& plan) {
    auto next_id = archstreamer::ClientId{1};
    for (const auto& client : plan.clients) {
        next_id = std::max<archstreamer::ClientId>(next_id, static_cast<archstreamer::ClientId>(client.client_id + 1));
    }
    return next_id;
}

archstreamer::SessionClientConnection* disconnected_player_for_reconnect(
    archstreamer::SessionPlan& plan,
    const archstreamer::ClientHello& hello) {
    for (auto& client : plan.clients) {
        if (client.connection_state != archstreamer::SessionConnectionState::Disconnected) {
            continue;
        }
        if (client.hello.requested_players == 0) {
            continue;
        }
        if (client.hello.username != hello.username) {
            continue;
        }
        if (client.hello.requested_players != hello.requested_players) {
            continue;
        }
        return &client;
    }

    return nullptr;
}

void poll_active_session_joins(
    archstreamer::TcpListener& listener,
    archstreamer::SessionPlan& plan,
    const archstreamer::GameList& game_list,
    const Args& args,
    std::size_t& media_index,
    ActiveMediaStreams& active_media) {
    auto stream = listener.accept_for(std::chrono::milliseconds(0));
    if (!stream.has_value()) {
        return;
    }

    std::cout << "Accepted active-session join candidate.\n";

    try {
        const auto first_payload = archstreamer::receive_control_payload(*stream);
        if (std::holds_alternative<archstreamer::ActiveSessionInfoRequest>(first_payload)) {
            stream->send_packet(archstreamer::serialize_packet(archstreamer::active_session_info_for(
                plan,
                args.video,
                args.audio)));
            return;
        }
        const auto* game_list_request = std::get_if<archstreamer::GameListRequest>(&first_payload);
        if (game_list_request == nullptr) {
            throw std::runtime_error("expected GameListRequest from active-session client");
        }
        stream->send_packet(archstreamer::serialize_packet(archstreamer::catalog_delta_for_request(game_list, *game_list_request)));

        const auto second_payload = archstreamer::receive_control_payload(*stream);
        const auto* hello = std::get_if<archstreamer::ClientHello>(&second_payload);
        if (hello == nullptr) {
            throw std::runtime_error("expected ClientHello from active-session client");
        }
        if (!archstreamer::valid_username(hello->username)) {
            throw std::runtime_error("active-session client supplied an invalid username");
        }
        if (!hello->selected_game_id.has_value() || *hello->selected_game_id != plan.selected_game_id) {
            throw std::runtime_error("active-session client selected a different game");
        }
        if (hello->session_mode != plan.session_mode) {
            throw std::runtime_error("active-session client selected a different session mode");
        }
        if (!archstreamer::valid_player_count(hello->requested_players)) {
            throw std::runtime_error("active-session client requested too many players");
        }
        if (hello->controllers.size() > hello->requested_players) {
            throw std::runtime_error("active-session client supplied controller metadata for unrequested players");
        }

        auto* reconnected_player = static_cast<archstreamer::SessionClientConnection*>(nullptr);
        auto client_id = next_session_client_id(plan);
        if (hello->requested_players > 0) {
            reconnected_player = disconnected_player_for_reconnect(plan, *hello);
            if (reconnected_player == nullptr) {
                throw std::runtime_error("active sessions only accept late viewers or reconnecting players");
            }
            client_id = reconnected_player->client_id;
        }

        auto welcome = archstreamer::HostWelcome{};
        welcome.client_id = client_id;
        welcome.max_players_for_client = archstreamer::MaxPlayersPerClient;
        welcome.host_is_player = plan.host_hello.has_value();
        stream->send_packet(archstreamer::serialize_packet(welcome));
        stream->send_packet(archstreamer::serialize_packet(plan.seats));
        stream->send_packet(archstreamer::serialize_packet(archstreamer::SessionReady{
            plan.selected_game_id,
            plan.session_mode,
            static_cast<std::uint8_t>(archstreamer::assigned_player_count(plan.seats)),
        }));

        auto destination_host = args.video_destination;
        if (!args.video_destination_explicit && !stream->peer_address().empty()) {
            destination_host = stream->peer_address();
        }
        auto endpoint = archstreamer::MediaEndpoint{};
        if (hello->wants_video || hello->wants_audio) {
            endpoint = active_media.start_for_client(client_id, destination_host, media_index);
            if (!endpoint.video_uri.empty() || !endpoint.audio_uri.empty()) {
                ++media_index;
                stream->send_packet(archstreamer::serialize_packet(endpoint));
            }
        }

        stream->send_packet(archstreamer::serialize_packet(archstreamer::SessionStarting{
            plan.selected_game_id,
            plan.session_mode,
            static_cast<std::uint8_t>(archstreamer::assigned_player_count(plan.seats)),
        }));

        if (reconnected_player != nullptr) {
            reconnected_player->hello = *hello;
            reconnected_player->stream = std::move(*stream);
            reconnected_player->connection_state = archstreamer::SessionConnectionState::Connected;
            reconnected_player->last_seen = std::chrono::steady_clock::now();
            reconnected_player->disconnected_at = {};
            std::cout
                << "Player " << static_cast<int>(client_id)
                << " reconnected username=" << hello->username << ".\n";
        } else {
            plan.clients.push_back(archstreamer::SessionClientConnection{
                client_id,
                *hello,
                std::move(*stream),
            });
            std::cout
                << "Late viewer " << static_cast<int>(client_id)
                << " joined username=" << hello->username << ".\n";
        }
    } catch (const std::exception& error) {
        try {
            stream->send_packet(archstreamer::serialize_packet(archstreamer::ErrorPacket{error.what()}));
        } catch (const std::exception&) {
        }
        std::cerr << "Rejected active-session join: " << error.what() << '\n';
    }
}

void print_usage() {
    std::cerr
        << "usage: host_runner [options] [game-index-or-id]\n"
        << "\n"
        << "options:\n"
        << "  --rom-root <path>   ROM root to scan. Default: " << DefaultRomRoot << "\n"
        << "  --meta-root <path>  Metadata root. Default: sibling Meta directory next to ROM root.\n"
        << "  --mode <mode>       singleplayer or multiplayer. Default: singleplayer\n"
        << "  --players <count>   Number of virtual pads to plug. Default: 1\n"
        << "  --list              List games and exit.\n"
        << "  --dry-run           Print selected launch config without launching.\n"
        << "  --pulse-input       Tap A on port 1 shortly after launch.\n"
        << "  --verbose           Pass --verbose to RetroArch.\n"
        << "  --control-port <port>\n"
        << "                      Wait for TCP session clients before launch.\n"
        << "  --input-port <port> Receive UDP ControllerInput packets on this port.\n"
        << "  --clients <count>   Maximum session clients to wait for. Default: 1\n"
        << "  --session-timeout <seconds>\n"
        << "                      Maximum time to wait for enough players. Default: 30\n"
        << "  --client-timeout <seconds>\n"
        << "                      Maximum time without client heartbeat during play. Default: 5\n"
        << "  --player-reconnect-timeout <seconds>\n"
        << "                      Time to reserve disconnected player seats. Default: 60\n"
        << "  --host-role <role>  player or viewer for the host. Default: player\n"
        << "  --video             Capture RetroArch video from a virtual display and stream RTP/H.264.\n"
        << "  --video-dest <ip>   Override video destination IP for all clients.\n"
        << "  --video-port <port> Base destination UDP video port. Default: 5004\n"
        << "  --audio             Capture host audio and stream RTP/Opus.\n"
        << "  --audio-port <port> Base destination UDP audio port. Default: 6004\n"
        << "  --audio-source <source>\n"
        << "                      Pulse/PipeWire source to capture. Default: audio server default source.\n"
        << "  --audio-backend <pulse|pipewire>\n"
        << "                      Audio capture backend. Default: pulse\n"
        << "  --virtual-display <display>\n"
        << "                      X display for virtual RetroArch output. Default: :99\n"
        << "  --video-resolution <WxH>\n"
        << "                      Virtual display resolution. Default: 1280x720\n"
        << "  --display-backend <auto|xvfb|xephyr>\n"
        << "                      Virtual display backend. Default: auto\n"
        << "  --bridge-controller <index>\n"
        << "                      Use a local SDL2 controller as host player 1.\n"
        << "  --ignore-controller <vid/pid>\n"
        << "                      Hide a physical controller from RetroArch SDL2, for example 0x054c/0x09cc.\n"
        << "  --retroarch-joypad-driver <driver>\n"
        << "                      RetroArch joypad driver for generated input config. Default: udev.\n"
        << "  --save-root <path>  Base save profile directory. Default: ~/.local/share/archstreamer/saves\n"
        << "  --username <name>   Save profile username. Default: $USER or local.\n"
        << "  --virtual-joypad-index <index>\n"
        << "                      RetroArch joypad index for the virtual pad. Default: 1 with bridge.\n";
}

Args parse_args(int argc, char** argv) {
    Args args;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--rom-root") {
            if (++i >= argc) {
                throw std::runtime_error("--rom-root requires a path");
            }
            args.rom_root = argv[i];
        } else if (arg == "--meta-root") {
            if (++i >= argc) {
                throw std::runtime_error("--meta-root requires a path");
            }
            args.meta_root = argv[i];
        } else if (arg == "--mode") {
            if (++i >= argc) {
                throw std::runtime_error("--mode requires singleplayer or multiplayer");
            }
            args.session_mode = parse_session_mode(argv[i]);
        } else if (arg == "--players") {
            if (++i >= argc) {
                throw std::runtime_error("--players requires a count");
            }
            const auto count = std::stoul(argv[i]);
            if (count == 0 || count > archstreamer::MaxRetroArchPorts) {
                throw std::runtime_error("--players must be between 1 and MaxRetroArchPorts");
            }
            args.players = static_cast<std::uint8_t>(count);
        } else if (arg == "--list") {
            args.list = true;
        } else if (arg == "--dry-run") {
            args.dry_run = true;
        } else if (arg == "--pulse-input") {
            args.pulse_input = true;
        } else if (arg == "--verbose") {
            args.verbose = true;
        } else if (arg == "--control-port") {
            if (++i >= argc) {
                throw std::runtime_error("--control-port requires a port");
            }
            args.control_port = static_cast<std::uint16_t>(std::stoul(argv[i]));
        } else if (arg == "--input-port") {
            if (++i >= argc) {
                throw std::runtime_error("--input-port requires a port");
            }
            args.input_port = static_cast<std::uint16_t>(std::stoul(argv[i]));
        } else if (arg == "--clients") {
            if (++i >= argc) {
                throw std::runtime_error("--clients requires a count");
            }
            args.clients = static_cast<std::uint8_t>(std::stoul(argv[i]));
        } else if (arg == "--session-timeout") {
            if (++i >= argc) {
                throw std::runtime_error("--session-timeout requires seconds");
            }
            args.session_timeout_seconds = static_cast<std::uint16_t>(std::stoul(argv[i]));
        } else if (arg == "--client-timeout") {
            if (++i >= argc) {
                throw std::runtime_error("--client-timeout requires seconds");
            }
            args.client_timeout_seconds = static_cast<std::uint16_t>(std::stoul(argv[i]));
            if (args.client_timeout_seconds == 0) {
                throw std::runtime_error("--client-timeout must be greater than zero");
            }
        } else if (arg == "--player-reconnect-timeout") {
            if (++i >= argc) {
                throw std::runtime_error("--player-reconnect-timeout requires seconds");
            }
            args.player_reconnect_timeout_seconds = static_cast<std::uint16_t>(std::stoul(argv[i]));
            if (args.player_reconnect_timeout_seconds == 0) {
                throw std::runtime_error("--player-reconnect-timeout must be greater than zero");
            }
        } else if (arg == "--host-role") {
            if (++i >= argc) {
                throw std::runtime_error("--host-role requires player or viewer");
            }
            args.host_role = parse_role(argv[i]);
        } else if (arg == "--video") {
            args.video = true;
        } else if (arg == "--video-dest") {
            if (++i >= argc) {
                throw std::runtime_error("--video-dest requires an IP address");
            }
            args.video = true;
            args.video_destination = argv[i];
            args.video_destination_explicit = true;
        } else if (arg == "--video-port") {
            if (++i >= argc) {
                throw std::runtime_error("--video-port requires a port");
            }
            args.video = true;
            args.video_port = static_cast<std::uint16_t>(std::stoul(argv[i]));
        } else if (arg == "--audio") {
            args.audio = true;
        } else if (arg == "--audio-port") {
            if (++i >= argc) {
                throw std::runtime_error("--audio-port requires a port");
            }
            args.audio = true;
            args.audio_port = static_cast<std::uint16_t>(std::stoul(argv[i]));
        } else if (arg == "--audio-source") {
            if (++i >= argc) {
                throw std::runtime_error("--audio-source requires a source name");
            }
            args.audio = true;
            args.audio_source = argv[i];
        } else if (arg == "--audio-backend") {
            if (++i >= argc) {
                throw std::runtime_error("--audio-backend requires pulse or pipewire");
            }
            args.audio = true;
            args.audio_backend = parse_audio_backend(argv[i]);
        } else if (arg == "--virtual-display") {
            if (++i >= argc) {
                throw std::runtime_error("--virtual-display requires a display name");
            }
            args.video = true;
            args.virtual_display = argv[i];
        } else if (arg == "--video-resolution") {
            if (++i >= argc) {
                throw std::runtime_error("--video-resolution requires a WxH value");
            }
            args.video = true;
            args.video_resolution = argv[i];
        } else if (arg == "--display-backend") {
            if (++i >= argc) {
                throw std::runtime_error("--display-backend requires auto, xvfb, or xephyr");
            }
            args.video = true;
            args.display_backend = parse_display_backend(argv[i]);
        } else if (arg == "--bridge-controller") {
            if (++i >= argc) {
                throw std::runtime_error("--bridge-controller requires a controller index");
            }
            args.bridge_controller_index = static_cast<std::size_t>(std::stoul(argv[i]));
        } else if (arg == "--ignore-controller") {
            if (++i >= argc) {
                throw std::runtime_error("--ignore-controller requires a VID/PID pair");
            }
            args.ignore_controller = argv[i];
        } else if (arg == "--retroarch-joypad-driver") {
            if (++i >= argc) {
                throw std::runtime_error("--retroarch-joypad-driver requires a driver name");
            }
            args.retroarch_joypad_driver = argv[i];
        } else if (arg == "--save-root") {
            if (++i >= argc) {
                throw std::runtime_error("--save-root requires a path");
            }
            args.save_root = argv[i];
        } else if (arg == "--username") {
            if (++i >= argc) {
                throw std::runtime_error("--username requires a name");
            }
            args.username = argv[i];
        } else if (arg == "--virtual-joypad-index") {
            if (++i >= argc) {
                throw std::runtime_error("--virtual-joypad-index requires an index");
            }
            args.virtual_joypad_index = static_cast<std::size_t>(std::stoul(argv[i]));
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(0);
        } else if (!args.selector.has_value()) {
            args.selector = arg;
        } else {
            throw std::runtime_error("unexpected extra argument: " + arg);
        }
    }

    if (args.clients == 0 || args.clients > archstreamer::MaxRemoteClients) {
        throw std::runtime_error("--clients must be between 1 and MaxRemoteClients");
    }

    return args;
}

void print_games(const archstreamer::GameList& list) {
    for (std::size_t i = 0; i < list.games.size(); ++i) {
        const auto& game = list.games[i];
        std::cout
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

std::string hex_vid_pid(std::uint16_t vendor_id, std::uint16_t product_id) {
    std::ostringstream out;
    out
        << "0x" << std::hex << std::setw(4) << std::setfill('0') << vendor_id
        << "/0x" << std::hex << std::setw(4) << std::setfill('0') << product_id;
    return out.str();
}

std::string sanitize_device_name(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        if (character == '"' || character == '\n' || character == '\r' || character == '\t') {
            result.push_back(' ');
        } else {
            result.push_back(character);
        }
    }
    return result;
}

std::string default_username() {
    if (const char* user = std::getenv("USER"); user != nullptr && archstreamer::valid_username(user)) {
        return user;
    }

    return "local";
}

std::optional<archstreamer::GameId> select_game(const archstreamer::GameList& list, const std::string& selector) {
    if (is_number(selector)) {
        const auto index = static_cast<std::size_t>(std::stoul(selector));
        if (index >= list.games.size()) {
            return std::nullopt;
        }

        return list.games[index].id;
    }

    for (const auto& game : list.games) {
        if (game.id == selector) {
            return game.id;
        }
    }

    return std::nullopt;
}

struct LaunchPlan {
    archstreamer::GameId game_id;
    archstreamer::GameSessionMode session_mode = archstreamer::GameSessionMode::SinglePlayer;
    std::uint8_t players = 1;
    std::string save_username;
    archstreamer::SeatAssignment seats;
    std::vector<archstreamer::VirtualGamepadIdentity> virtual_identities;
};

archstreamer::SeatAssignment direct_seat_assignment(std::uint8_t players) {
    archstreamer::SeatAssignment assignment;
    for (archstreamer::LocalPlayerIndex player = 0; player < players; ++player) {
        assignment.seats.push_back(archstreamer::PlayerSeat{archstreamer::HostClientId, player, player});
    }

    return assignment;
}

void validate_direct_launch(
    archstreamer::GameSessionMode mode,
    std::uint8_t players,
    const archstreamer::GameInfo& game) {
    if (!archstreamer::valid_game_player_limits(game.min_players, game.max_players)) {
        throw std::runtime_error("selected game has invalid player metadata");
    }
    if (mode == archstreamer::GameSessionMode::SinglePlayer && !game.supports_singleplayer) {
        throw std::runtime_error("selected game does not support singleplayer");
    }
    if (mode == archstreamer::GameSessionMode::Multiplayer && !game.supports_multiplayer) {
        throw std::runtime_error("selected game does not support multiplayer");
    }
    if (players > game.max_players) {
        throw std::runtime_error("too many players selected for game");
    }

    const auto required = archstreamer::required_player_count(mode, game);
    if (players < required) {
        std::ostringstream message;
        message
            << "not enough players selected for " << archstreamer::session_mode_name(mode)
            << " session: need " << static_cast<int>(required)
            << ", got " << static_cast<int>(players);
        throw std::runtime_error(message.str());
    }
}

std::optional<std::string> sdl_ignore_list_for_session(const archstreamer::SessionPlan& plan) {
    std::vector<std::string> ignored;
    std::string result;
    const auto add_controller = [&](const archstreamer::ControllerInfo& controller) {
        if (controller.vendor_id == 0 || controller.product_id == 0) {
            return;
        }
        const auto vid_pid = hex_vid_pid(controller.vendor_id, controller.product_id);
        if (std::find(ignored.begin(), ignored.end(), vid_pid) != ignored.end()) {
            return;
        }
        ignored.push_back(vid_pid);
        if (!result.empty()) {
            result += ",";
        }
        result += vid_pid;
    };

    if (plan.host_hello.has_value()) {
        for (const auto& controller : plan.host_hello->controllers) {
            add_controller(controller);
        }
    }
    for (const auto& client : plan.clients) {
        for (const auto& controller : client.hello.controllers) {
            add_controller(controller);
        }
    }

    if (result.empty()) {
        return std::nullopt;
    }

    return result;
}

void pulse_a(archstreamer::LinuxUinputGamepadBus& gamepads) {
    ControllerState state;
    state.timestamp_us = 1;
    state.buttons = ButtonA;
    gamepads.update(0, state);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    state.timestamp_us = 2;
    state.buttons = 0;
    gamepads.update(0, state);
}

std::filesystem::path write_virtual_pad_autoconfig(
    const archstreamer::VirtualGamepadIdentity& identity,
    const std::string& joypad_driver,
    archstreamer::RetroArchPort port) {
    const auto directory = std::filesystem::temp_directory_path() / "archstreamer-retroarch-autoconfig" / joypad_driver;

    auto port_identity = identity;
    port_identity.name += " P" + std::to_string(static_cast<int>(port) + 1);
    port_identity.product_id = static_cast<std::uint16_t>(port_identity.product_id + port);

    const auto device_name = sanitize_device_name(port_identity.name);
    const auto path = directory / (device_name + ".cfg");
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        throw std::runtime_error("failed to write RetroArch virtual pad autoconfig");
    }

    file
        << "input_driver = \"" << joypad_driver << "\"\n"
        << "input_device = \"" << device_name << "\"\n"
        << "input_vendor_id = \"" << port_identity.vendor_id << "\"\n"
        << "input_product_id = \"" << port_identity.product_id << "\"\n"
        << "input_b_btn = \"0\"\n"
        << "input_a_btn = \"1\"\n"
        << "input_y_btn = \"2\"\n"
        << "input_x_btn = \"3\"\n"
        << "input_l_btn = \"4\"\n"
        << "input_r_btn = \"5\"\n"
        << "input_select_btn = \"6\"\n"
        << "input_start_btn = \"7\"\n"
        << "input_l3_btn = \"9\"\n"
        << "input_r3_btn = \"10\"\n"
        << "input_l2_axis = \"+2\"\n"
        << "input_r2_axis = \"+5\"\n"
        << "input_l_x_minus_axis = \"-0\"\n"
        << "input_l_x_plus_axis = \"+0\"\n"
        << "input_l_y_minus_axis = \"-1\"\n"
        << "input_l_y_plus_axis = \"+1\"\n"
        << "input_r_x_minus_axis = \"-3\"\n"
        << "input_r_x_plus_axis = \"+3\"\n"
        << "input_r_y_minus_axis = \"-4\"\n"
        << "input_r_y_plus_axis = \"+4\"\n"
        << "input_up_btn = \"11\"\n"
        << "input_down_btn = \"12\"\n"
        << "input_left_btn = \"13\"\n"
        << "input_right_btn = \"14\"\n"
        << "input_left_axis = \"-6\"\n"
        << "input_right_axis = \"+6\"\n"
        << "input_up_axis = \"-7\"\n"
        << "input_down_axis = \"+7\"\n";

    return directory.parent_path();
}

archstreamer::VirtualGamepadIdentity identity_for_port(
    const std::vector<archstreamer::VirtualGamepadIdentity>& identities,
    archstreamer::RetroArchPort port) {
    if (port < identities.size()) {
        auto identity = identities[port];
        if (identity.name.empty()) {
            identity.name = "ArchStreamer Virtual Gamepad";
        }
        return identity;
    }

    return archstreamer::VirtualGamepadIdentity{};
}

std::filesystem::path write_retroarch_input_override(
    std::size_t first_virtual_joypad_index,
    const std::vector<archstreamer::VirtualGamepadIdentity>& identities,
    const std::string& joypad_driver,
    archstreamer::RetroArchPort players,
    const archstreamer::SaveProfile& save_profile) {
    const auto directory = std::filesystem::temp_directory_path() / "archstreamer-retroarch-config";
    std::filesystem::create_directories(directory);

    const auto autoconfig_directory =
        std::filesystem::temp_directory_path() / "archstreamer-retroarch-autoconfig";
    const auto autoconfig_driver_directory = autoconfig_directory / joypad_driver;
    std::filesystem::remove_all(autoconfig_directory);
    std::filesystem::create_directories(autoconfig_driver_directory);

    for (archstreamer::RetroArchPort port = 0; port < players; ++port) {
        write_virtual_pad_autoconfig(identity_for_port(identities, port), joypad_driver, port);
    }
    const auto path = directory / "input_override.cfg";
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        throw std::runtime_error("failed to write RetroArch input override");
    }

    file
        << "config_save_on_exit = \"false\"\n"
        << "input_joypad_driver = \"" << joypad_driver << "\"\n"
        << "input_max_users = \"" << static_cast<int>(players) << "\"\n"
        << "input_autodetect_enable = \"true\"\n"
        << "notification_show_autoconfig = \"false\"\n"
        << "joypad_autoconfig_dir = \"" << autoconfig_directory.string() << "\"\n";

    file
        << "savefile_directory = \"" << save_profile.savefile_directory.string() << "\"\n"
        << "savestate_directory = \"" << save_profile.savestate_directory.string() << "\"\n"
        << "sort_savefiles_enable = \"false\"\n"
        << "sort_savefiles_by_content_enable = \"false\"\n"
        << "sort_savestates_enable = \"false\"\n"
        << "sort_savestates_by_content_enable = \"false\"\n";

    for (archstreamer::RetroArchPort port = 0; port < players; ++port) {
        const auto player = static_cast<int>(port) + 1;
        const auto joypad_index = first_virtual_joypad_index + port;
        file
            << "input_player" << player << "_joypad_index = \"" << joypad_index << "\"\n"
            << "input_player" << player << "_b_btn = \"0\"\n"
            << "input_player" << player << "_a_btn = \"1\"\n"
            << "input_player" << player << "_y_btn = \"2\"\n"
            << "input_player" << player << "_x_btn = \"3\"\n"
            << "input_player" << player << "_l_btn = \"4\"\n"
            << "input_player" << player << "_r_btn = \"5\"\n"
            << "input_player" << player << "_select_btn = \"6\"\n"
            << "input_player" << player << "_start_btn = \"7\"\n"
            << "input_player" << player << "_l3_btn = \"9\"\n"
            << "input_player" << player << "_r3_btn = \"10\"\n"
            << "input_player" << player << "_l2_axis = \"+2\"\n"
            << "input_player" << player << "_r2_axis = \"+5\"\n"
            << "input_player" << player << "_l_x_minus_axis = \"-0\"\n"
            << "input_player" << player << "_l_x_plus_axis = \"+0\"\n"
            << "input_player" << player << "_l_y_minus_axis = \"-1\"\n"
            << "input_player" << player << "_l_y_plus_axis = \"+1\"\n"
            << "input_player" << player << "_r_x_minus_axis = \"-3\"\n"
            << "input_player" << player << "_r_x_plus_axis = \"+3\"\n"
            << "input_player" << player << "_r_y_minus_axis = \"-4\"\n"
            << "input_player" << player << "_r_y_plus_axis = \"+4\"\n"
            << "input_player" << player << "_up_btn = \"11\"\n"
            << "input_player" << player << "_down_btn = \"12\"\n"
            << "input_player" << player << "_left_btn = \"13\"\n"
            << "input_player" << player << "_right_btn = \"14\"\n"
            << "input_player" << player << "_left_axis = \"-6\"\n"
            << "input_player" << player << "_right_axis = \"+6\"\n"
            << "input_player" << player << "_up_axis = \"-7\"\n"
            << "input_player" << player << "_down_axis = \"+7\"\n";
    }

    return path;
}

class LocalControllerBridge {
public:
    explicit LocalControllerBridge(archstreamer::ControllerDevice device)
        : device_(std::move(device)) {
        std::cout
            << "Bridging local controller "
            << ": " << device_.name
            << " [id=" << device_.id << "]"
            << " vid/pid=" << hex_vid_pid(device_.vendor_id, device_.product_id)
            << "\n";

        backend_.open_selected({device_.id});
    }

    const archstreamer::ControllerDevice& device() const {
        return device_;
    }

    void update(archstreamer::InputRouter& input_router) {
        const auto state = backend_.poll(0);
        if (state.has_value()) {
            input_router.route(archstreamer::ControllerInput{
                archstreamer::HostClientId,
                0,
                *state,
            });
        }
    }

private:
    archstreamer::ControllerBackend backend_;
    archstreamer::ControllerDevice device_;
};

archstreamer::ControllerDevice local_bridge_device_for(std::size_t selected_index) {
    archstreamer::ControllerBackend backend;
    const auto devices = backend.list_devices();
    if (devices.empty()) {
        throw std::runtime_error("no SDL2 game controllers detected for local bridge");
    }
    if (selected_index >= devices.size()) {
        throw std::runtime_error("local bridge controller index is out of range");
    }

    return devices[selected_index];
}

class NetworkInputReceiver {
public:
    NetworkInputReceiver(std::uint16_t port, archstreamer::InputRouter& input_router)
        : input_router_(input_router) {
        socket_.bind_any(port);
        socket_.set_nonblocking(true);
        std::cout << "Receiving UDP controller input on port " << port << '\n';
    }

    void poll() {
        while (true) {
            const auto bytes = socket_.receive();
            if (!bytes.has_value()) {
                return;
            }

            try {
                auto payload = archstreamer::deserialize_packet(*bytes);
                if (auto* input = std::get_if<archstreamer::ControllerInput>(&payload); input != nullptr) {
                    input_router_.route(*input);
                }
            } catch (const std::exception& error) {
                std::cerr << "Ignoring bad input packet: " << error.what() << '\n';
            }
        }
    }

private:
    archstreamer::UdpSocket socket_;
    archstreamer::InputRouter& input_router_;
};

class SessionControlMonitor {
public:
    SessionControlMonitor(
        archstreamer::SessionPlan& plan,
        archstreamer::InputRouter& input_router,
        ActiveMediaStreams& active_media,
        std::chrono::seconds heartbeat_timeout,
        std::chrono::seconds reconnect_timeout)
        : plan_(plan),
          input_router_(input_router),
          active_media_(active_media),
          heartbeat_timeout_(heartbeat_timeout),
          reconnect_timeout_(reconnect_timeout) {
        const auto now = std::chrono::steady_clock::now();
        for (auto& client : plan_.clients) {
            client.last_seen = now;
        }
    }

    std::optional<std::string> poll() {
        const auto now = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < plan_.clients.size();) {
            auto& client = plan_.clients[i];
            if (client.connection_state == archstreamer::SessionConnectionState::Disconnected) {
                if (now - client.disconnected_at > reconnect_timeout_) {
                    return client_label(client) + " reconnect timed out";
                }
                ++i;
                continue;
            }

            bool removed_current = false;
            while (client.stream.readable()) {
                const auto packet = client.stream.receive_packet();
                if (!packet.has_value()) {
                    if (remove_viewer(i, "disconnected")) {
                        removed_current = true;
                        break;
                    }
                    mark_player_disconnected(client, "disconnected");
                    ++i;
                    removed_current = true;
                    break;
                }

                const auto payload = archstreamer::deserialize_packet(*packet);
                if (const auto* heartbeat = std::get_if<archstreamer::ViewerHeartbeat>(&payload); heartbeat != nullptr) {
                    if (heartbeat->client_id == client.client_id) {
                        client.last_seen = now;
                    }
                }
            }
            if (removed_current) {
                continue;
            }

            if (client.stream.peer_closed()) {
                if (remove_viewer(i, "disconnected")) {
                    continue;
                }
                mark_player_disconnected(client, "disconnected");
                ++i;
                continue;
            }
            if (now - client.last_seen > heartbeat_timeout_) {
                if (remove_viewer(i, "heartbeat timed out")) {
                    continue;
                }
                mark_player_disconnected(client, "heartbeat timed out");
                ++i;
                continue;
            }
            ++i;
        }

        return std::nullopt;
    }

private:
    bool remove_viewer(std::size_t index, std::string_view reason) {
        if (plan_.clients[index].hello.requested_players != 0) {
            return false;
        }

        std::cerr
            << "Removing viewer " << static_cast<int>(plan_.clients[index].client_id)
            << " (" << plan_.clients[index].hello.username << "): "
            << reason << '\n';
        active_media_.stop_for_client(plan_.clients[index].client_id);
        plan_.clients.erase(plan_.clients.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }

    void mark_player_disconnected(archstreamer::SessionClientConnection& client, std::string_view reason) {
        active_media_.stop_for_client(client.client_id);
        client.connection_state = archstreamer::SessionConnectionState::Disconnected;
        client.disconnected_at = std::chrono::steady_clock::now();
        input_router_.neutralize_client(client.client_id);
        std::cerr
            << "Player " << static_cast<int>(client.client_id)
            << " (" << client.hello.username << ") disconnected: "
            << reason << "; reserving seats for "
            << reconnect_timeout_.count() << "s\n";
    }

    static std::string client_label(const archstreamer::SessionClientConnection& client) {
        std::ostringstream out;
        out << "client " << static_cast<int>(client.client_id) << " (" << client.hello.username << ")";
        return out.str();
    }

    archstreamer::SessionPlan& plan_;
    archstreamer::InputRouter& input_router_;
    ActiveMediaStreams& active_media_;
    std::chrono::seconds heartbeat_timeout_;
    std::chrono::seconds reconnect_timeout_;
};

} // namespace

int main(int argc, char** argv) {
    using namespace archstreamer;

    try {
        auto args = parse_args(argc, argv);
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
        std::signal(SIGPIPE, SIG_IGN);
        auto catalog = scan_game_catalog(args.rom_root, LibretroCoreRegistry::ubuntu_defaults(), args.meta_root);
        const auto list = catalog.list();

        if (args.audio && args.audio_backend == AudioCaptureBackend::Pulse && args.audio_source.empty()) {
            args.audio_source = default_pulse_monitor_source();
            if (args.audio_source.empty()) {
                std::cerr
                    << "Warning: could not determine default Pulse monitor source; "
                    << "audio capture will use the audio server default source.\n";
            }
        }

        if (list.games.empty()) {
            std::cerr << "No supported games found under " << args.rom_root << '\n';
            return 1;
        }

        if (args.list || (!args.selector.has_value() && !args.control_port.has_value())) {
            std::cout << "Found " << list.games.size() << " supported games under " << args.rom_root << ".\n";
            print_games(list);
            return args.list ? 0 : 2;
        }

        auto bridge_device = std::optional<ControllerDevice>{};
        if (args.host_role == ParticipantRole::Viewer && args.bridge_controller_index.has_value()) {
            throw std::runtime_error("--bridge-controller cannot be used with --host-role viewer");
        }
        if (args.host_role == ParticipantRole::Player && args.bridge_controller_index.has_value()) {
            bridge_device = local_bridge_device_for(*args.bridge_controller_index);
            if (args.retroarch_joypad_driver == "udev" && !args.ignore_controller.has_value()) {
                args.retroarch_joypad_driver = "sdl2";
            }
        }

        auto launch_plan = LaunchPlan{};
        auto session_plan = std::optional<SessionPlan>{};
        if (args.control_port.has_value()) {
            if (!args.input_port.has_value()) {
                args.input_port = 45454;
            }
            auto host_hello = std::optional<ClientHello>{};
            if (bridge_device.has_value()) {
                if (!args.selector.has_value()) {
                    throw std::runtime_error("--bridge-controller in session mode requires a game selector");
                }
                const auto game_id = select_game(list, *args.selector);
                if (!game_id.has_value()) {
                    std::cerr << "Game not found: " << *args.selector << '\n';
                    return 1;
                }
                const auto selected_game = game_info_for(list, *game_id);
                if (!selected_game.has_value()) {
                    throw std::runtime_error("selected game is missing from game list");
                }
                if (!valid_game_player_limits(selected_game->min_players, selected_game->max_players)) {
                    throw std::runtime_error("selected game has invalid player metadata");
                }
                if (args.session_mode == GameSessionMode::SinglePlayer && !selected_game->supports_singleplayer) {
                    throw std::runtime_error("selected game does not support singleplayer");
                }
                if (args.session_mode == GameSessionMode::Multiplayer && !selected_game->supports_multiplayer) {
                    throw std::runtime_error("selected game does not support multiplayer");
                }
                if (selected_game->max_players < 1) {
                    throw std::runtime_error("selected game does not have room for the host player");
                }
                if (args.username.empty()) {
                    args.username = default_username();
                }
                if (!valid_username(args.username)) {
                    throw std::runtime_error("--username must be 1-64 characters and contain only letters, numbers, underscores, or hyphens");
                }
                host_hello = ClientHello{
                    args.username,
                    args.username,
                    *game_id,
                    args.session_mode,
                    1,
                    std::vector<ControllerInfo>{
                        ControllerInfo{
                            0,
                            bridge_device->name,
                            bridge_device->guid,
                            bridge_device->vendor_id,
                            bridge_device->product_id,
                        },
                    },
                    true,
                    true,
                };
            }
            HostSessionService session_service(
                *args.control_port,
                args.clients,
                list,
                std::chrono::seconds(args.session_timeout_seconds),
                std::move(host_hello));
            session_plan = session_service.wait_for_ready_session();
            if (args.retroarch_joypad_driver == "udev" && !args.ignore_controller.has_value()) {
                args.retroarch_joypad_driver = "sdl2";
            }
            if (!args.ignore_controller.has_value()) {
                args.ignore_controller = sdl_ignore_list_for_session(*session_plan);
            }
            launch_plan.game_id = session_plan->selected_game_id;
            launch_plan.session_mode = session_plan->session_mode;
            launch_plan.players = static_cast<std::uint8_t>(assigned_player_count(session_plan->seats));
            launch_plan.save_username = session_plan->save_username;
            launch_plan.seats = session_plan->seats;
            launch_plan.virtual_identities = virtual_identities_for_session(*session_plan);
        } else {
            const auto game_id = select_game(list, *args.selector);
            if (!game_id.has_value()) {
                std::cerr << "Game not found: " << *args.selector << '\n';
                return 1;
            }
            const auto selected_game = game_info_for(list, *game_id);
            if (!selected_game.has_value()) {
                throw std::runtime_error("selected game is missing from game list");
            }
            validate_direct_launch(args.session_mode, args.players, *selected_game);

            if (args.username.empty()) {
                args.username = default_username();
            }
            if (!valid_username(args.username)) {
                throw std::runtime_error("--username must be 1-64 characters and contain only letters, numbers, underscores, or hyphens");
            }

            auto virtual_identity = VirtualGamepadIdentity{};
            if (bridge_device.has_value()) {
                virtual_identity.name = "ArchStreamer " + sanitize_device_name(bridge_device->name);
            }

            launch_plan.game_id = *game_id;
            launch_plan.session_mode = args.session_mode;
            launch_plan.players = args.players;
            launch_plan.save_username = args.username;
            launch_plan.seats = direct_seat_assignment(args.players);
            launch_plan.virtual_identities.assign(args.players, virtual_identity);
        }

        if (launch_plan.save_username.empty()) {
            launch_plan.save_username = default_username();
        }
        if (!valid_username(launch_plan.save_username)) {
            throw std::runtime_error("save username must be 1-64 characters and contain only letters, numbers, underscores, or hyphens");
        }
        if (launch_plan.virtual_identities.size() < launch_plan.players) {
            launch_plan.virtual_identities.resize(launch_plan.players);
        }

        const auto save_profile = prepare_save_profile(args.save_root, launch_plan.save_username);

        auto launch_config = catalog.launch_config_for(launch_plan.game_id);
        launch_config.extra_args.insert(launch_config.extra_args.begin(), "-f");
        if (args.verbose) {
            launch_config.extra_args.insert(launch_config.extra_args.begin(), "--verbose");
        }
        const auto default_joypad_index =
            (args.bridge_controller_index.has_value() || args.control_port.has_value()) &&
                args.retroarch_joypad_driver == "sdl2" ? 0 : 1;
        const auto virtual_joypad_index = args.virtual_joypad_index.value_or(default_joypad_index);
        const auto runtime_override = write_retroarch_input_override(
            virtual_joypad_index,
            launch_plan.virtual_identities,
            args.retroarch_joypad_driver,
            launch_plan.players,
            save_profile);
        launch_config.extra_args.push_back("-c");
        launch_config.extra_args.push_back(runtime_override.string());
        if (bridge_device.has_value() && !args.ignore_controller.has_value()) {
            if (bridge_device->vendor_id != 0 && bridge_device->product_id != 0) {
                args.ignore_controller = hex_vid_pid(bridge_device->vendor_id, bridge_device->product_id);
            }
        }
        if (args.ignore_controller.has_value()) {
            launch_config.environment.emplace_back("SDL_GAMECONTROLLER_IGNORE_DEVICES", *args.ignore_controller);
            if (args.retroarch_joypad_driver != "sdl2") {
                std::cerr
                    << "Warning: SDL_GAMECONTROLLER_IGNORE_DEVICES only affects RetroArch when "
                    << "--retroarch-joypad-driver is sdl2.\n";
            }
        }
        auto media_destinations = std::vector<std::pair<ClientId, std::string>>{};
        auto media_streams = std::vector<GStreamerMediaStream>{};
        if (args.video || args.audio) {
            if (session_plan.has_value()) {
                media_destinations = media_destinations_for_session(args, *session_plan);
                media_streams = media_streams_for_dry_run(args, media_destinations);
            } else {
                media_destinations.emplace_back(HostClientId, args.video_destination);
                media_streams = media_streams_for_dry_run(args, media_destinations);
            }
        }
        if (args.video) {
            launch_config.environment.emplace_back("DISPLAY", args.virtual_display);
        }

        std::cout
            << "Selected game: " << launch_plan.game_id
            << "\nRetroArch: " << launch_config.retroarch_path
            << "\nCore:      " << launch_config.core_path
            << "\nContent:   " << launch_config.content_path
            << "\nMode:      " << session_mode_name(launch_plan.session_mode)
            << "\nPlayers:   " << static_cast<int>(launch_plan.players)
            << "\nHostRole:  " << role_name(args.host_role)
            << "\nJoypad:    " << args.retroarch_joypad_driver
            << "\nUser:      " << save_profile.username
            << "\nSaves:     " << save_profile.savefile_directory
            << "\nStates:    " << save_profile.savestate_directory
            << '\n';
        for (const auto& stream : media_streams) {
            if (!stream.endpoint.video_uri.empty()) {
                std::cout
                    << "Video:     client " << static_cast<int>(stream.client_id)
                    << " " << stream.endpoint.video_uri
                    << " from display " << args.virtual_display
                    << " at " << args.video_resolution << '\n';
            }
            if (!stream.endpoint.audio_uri.empty()) {
                std::cout
                    << "Audio:     client " << static_cast<int>(stream.client_id)
                    << " " << stream.endpoint.audio_uri;
                if (!args.audio_source.empty()) {
                    std::cout << " from " << args.audio_source;
                }
                std::cout << '\n';
            }
        }
        for (RetroArchPort port = 0; port < launch_plan.players; ++port) {
            const auto identity = identity_for_port(launch_plan.virtual_identities, port);
            std::cout
                << "Virtual:   P" << static_cast<int>(port) + 1
                << " " << identity.name << " P" << static_cast<int>(port) + 1
                << " " << hex_vid_pid(
                    identity.vendor_id,
                    static_cast<std::uint16_t>(identity.product_id + port))
                << '\n';
        }
        if (args.ignore_controller.has_value()) {
            std::cout << "Ignoring:  " << *args.ignore_controller << " for SDL2 controller discovery\n";
        }

        if (args.dry_run) {
            if (session_plan.has_value()) {
                for (const auto& stream : media_streams) {
                    send_media_endpoint_to_client(*session_plan, stream.client_id, stream.endpoint);
                }
                send_session_starting_to_clients(*session_plan);
                send_session_ended_to_clients(*session_plan, "dry run complete");
            }
            return 0;
        }

        LinuxUinputGamepadBus gamepads(launch_plan.virtual_identities);
        for (RetroArchPort port = 0; port < launch_plan.players; ++port) {
            gamepads.plug(port);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(750));

        InputRouter input_router(gamepads);
        input_router.set_seat_assignment(launch_plan.seats);

        auto network_receiver = std::optional<NetworkInputReceiver>{};
        if (args.input_port.has_value()) {
            network_receiver.emplace(*args.input_port, input_router);
        }

        auto virtual_display = std::optional<VirtualDisplayProcess>{};
        auto video_fanout = std::optional<GStreamerVideoFanout>{};
        auto audio_fanout = std::optional<GStreamerAudioFanout>{};
        auto media_index = media_destinations.size();
        if (args.video) {
            virtual_display.emplace();
            virtual_display->start(args.display_backend, args.virtual_display, args.video_resolution);
            video_fanout.emplace();
            const auto video_streams = video_fanout->start(
                args.virtual_display,
                video_requests_from_media_destinations(args, media_destinations));
            for (const auto& stream : video_streams) {
                for (auto& media_stream : media_streams) {
                    if (media_stream.client_id == stream.client_id) {
                        media_stream.endpoint.video_uri = stream.endpoint.video_uri;
                    }
                }
            }
        }
        if (args.audio) {
            audio_fanout.emplace();
            const auto audio_streams = audio_fanout->start(
                args.audio_backend,
                args.audio_source,
                audio_requests_from_media_destinations(args, media_destinations));
            for (const auto& stream : audio_streams) {
                for (auto& media_stream : media_streams) {
                    if (media_stream.client_id == stream.client_id) {
                        media_stream.endpoint.audio_uri = stream.endpoint.audio_uri;
                    }
                }
            }
        }
        if (session_plan.has_value()) {
            for (const auto& stream : media_streams) {
                if (!stream.endpoint.video_uri.empty() || !stream.endpoint.audio_uri.empty()) {
                    send_media_endpoint_to_client(*session_plan, stream.client_id, stream.endpoint);
                }
            }
        }

        if (session_plan.has_value()) {
            send_session_starting_to_clients(*session_plan);
        }
        auto active_media = ActiveMediaStreams(args, video_fanout, audio_fanout);
        auto late_viewer_listener = std::optional<TcpListener>{};
        if (session_plan.has_value() && args.control_port.has_value()) {
            late_viewer_listener.emplace(*args.control_port);
            std::cout
                << "Accepting late viewers and reconnecting players on TCP port "
                << *args.control_port << ".\n";
        }
        auto session_monitor = std::optional<SessionControlMonitor>{};
        if (session_plan.has_value()) {
            session_monitor.emplace(
                *session_plan,
                input_router,
                active_media,
                std::chrono::seconds(args.client_timeout_seconds),
                std::chrono::seconds(args.player_reconnect_timeout_seconds));
        }

        auto local_bridge = std::optional<LocalControllerBridge>{};
        if (bridge_device.has_value()) {
            local_bridge.emplace(*bridge_device);
        }

        PosixRetroArchProcess retroarch;
        retroarch.launch(launch_config);

        if (args.pulse_input && launch_plan.players > 0) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            pulse_a(gamepads);
        }

        while (!stop_requested && retroarch.running()) {
            if (local_bridge.has_value()) {
                local_bridge->update(input_router);
            }
            if (network_receiver.has_value()) {
                network_receiver->poll();
            }
            if (late_viewer_listener.has_value() && session_plan.has_value()) {
                poll_active_session_joins(
                    *late_viewer_listener,
                    *session_plan,
                    list,
                    args,
                    media_index,
                    active_media);
            }
            if (session_monitor.has_value()) {
                if (const auto reason = session_monitor->poll(); reason.has_value()) {
                    std::cerr << "Stopping session: " << *reason << '\n';
                    stop_requested = true;
                    break;
                }
            }

            if (local_bridge.has_value() || network_receiver.has_value()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        }

        retroarch.stop();
        if (audio_fanout.has_value()) {
            audio_fanout->stop();
        }
        if (video_fanout.has_value()) {
            video_fanout->stop();
        }
        if (virtual_display.has_value()) {
            virtual_display->stop();
        }
        if (session_plan.has_value()) {
            send_session_ended_to_clients(*session_plan, stop_requested ? "host stopped" : "retroarch exited");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "host_runner: " << error.what() << '\n';
        return 1;
    }
}
