#include "tools/cli.hpp"

#include "common/cli_common.hpp"
#include "host/save_profile.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string_view>

namespace archstreamer {
namespace {

VirtualDisplayBackend parse_display_backend(std::string_view value) {
    if (value == "auto") {
        return VirtualDisplayBackend::None;
    }
    if (value == "xvfb") {
        return VirtualDisplayBackend::Xvfb;
    }
    if (value == "xephyr") {
        return VirtualDisplayBackend::Xephyr;
    }

    throw std::runtime_error("--display-backend must be auto, xvfb, or xephyr");
}

AudioCaptureBackend parse_audio_backend(std::string_view value) {
    if (value == "pulse" || value == "pulseaudio") {
        return AudioCaptureBackend::Pulse;
    }
    if (value == "pipewire" || value == "pw") {
        return AudioCaptureBackend::PipeWire;
    }

    throw std::runtime_error("--audio-backend must be pulse or pipewire");
}

} // namespace

HostRunnerCli::HostRunnerCli(std::ostream& out, std::ostream& err)
    : out_(out), err_(err) {
}

void HostRunnerCli::print_usage() const {
    out_
        << "usage: host_runner [options] [game-index-or-id]\n"
        << "\n"
        << "options:\n"
        << "  --rom-root <path>   ROM root to scan. Default: " << DefaultRomRoot << "\n"
        << "  --meta-root <path>  Metadata root. Default: sibling Meta directory next to ROM root.\n"
        << "  --art-root <path>   Artwork root served to clients. Default: sibling Art next to ROM root.\n"
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

HostAppConfig HostRunnerCli::parse(int argc, char** argv) const {
    HostAppConfig args;

    auto if_throw = [argc](int& i, std::string arg) -> bool {
        if (++i >= argc) {
            throw std::runtime_error(arg);
        }
        return false;
    };

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        
        if (arg == "--list") {
            args.list = true;
        } else if (arg == "--dry-run") {
            args.dry_run = true;
        } else if (arg == "--pulse-input") {
            args.pulse_input = true;
        } else if (arg == "--verbose") {
            args.verbose = true;
        } else if (arg == "--video") {
            args.video = true;
        } else if (arg == "--audio") {
            args.audio = true;
        } else if (arg == "--rom-root") {
            if_throw(i, "--rom-root requires a path");
            args.rom_root = argv[i];
        } else if (arg == "--meta-root") {
            if_throw(i, "--meta-root requires a path");
            args.meta_root = argv[i];
        } else if (arg == "--art-root") {
            if_throw(i, "--art-root requires a path");
            args.art_root = argv[i];
        } else if (arg == "--mode") {
            if_throw(i, "--mode requires singleplayer or multiplayer");
            args.session_mode = parse_session_mode(argv[i]);
        } else if (arg == "--players") {
            if_throw(i, "--players requires a count");
           
            const auto count = std::stoul(argv[i]);
            if (count == 0 || count > MaxRetroArchPorts) {
                throw std::runtime_error("--players must be between 1 and MaxRetroArchPorts");
            }
            args.players = static_cast<std::uint8_t>(count);
        } else if (arg == "--control-port") {
            if_throw(i, "--control-port requires a port");
            args.control_port = static_cast<std::uint16_t>(std::stoul(argv[i]));
        } else if (arg == "--input-port") {
            if_throw(i, "--input-port requires a port");
            args.input_port = static_cast<std::uint16_t>(std::stoul(argv[i]));
        } else if (arg == "--clients") {
            if_throw(i, "--clients requires a count");
            args.clients = static_cast<std::uint8_t>(std::stoul(argv[i]));
        } else if (arg == "--session-timeout") {
            if_throw(i, "--session-timeout requires seconds");
            args.session_timeout_seconds = static_cast<std::uint16_t>(std::stoul(argv[i]));
        } else if (arg == "--client-timeout") {
            if_throw(i, "--client-timeout requires seconds");
            args.client_timeout_seconds = static_cast<std::uint16_t>(std::stoul(argv[i]));
            if (args.client_timeout_seconds == 0) {
                throw std::runtime_error("--client-timeout must be greater than zero");
            }
        } else if (arg == "--player-reconnect-timeout") {
            if_throw(i, "--player-reconnect-timeout requires seconds");
            args.player_reconnect_timeout_seconds = static_cast<std::uint16_t>(std::stoul(argv[i]));
            if (args.player_reconnect_timeout_seconds == 0) {
                throw std::runtime_error("--player-reconnect-timeout must be greater than zero");
            }
        } else if (arg == "--host-role") {
            if_throw(i, "--host-role requires player or viewer");
            args.host_role = parse_participant_role(argv[i]);
        } else if (arg == "--video-dest") {
            if_throw(i, "--video-dest requires an IP address");
            args.video = true;
            args.video_destination = argv[i];
            args.video_destination_explicit = true;
        } else if (arg == "--video-port") {
            if_throw(i, "--video-port requires a port");
            args.video = true;
            args.video_port = static_cast<std::uint16_t>(std::stoul(argv[i]));
        } else if (arg == "--audio-port") {
            if_throw(i, "--audio-port requires a port");
            args.audio = true;
            args.audio_port = static_cast<std::uint16_t>(std::stoul(argv[i]));
        } else if (arg == "--audio-source") {
            if_throw(i, "--audio-source requires a source name");
            args.audio = true;
            args.audio_source = argv[i];
        } else if (arg == "--audio-backend") {
            if_throw(i, "--audio-backend requires pulse or pipewire");
            args.audio = true;
            args.audio_backend = parse_audio_backend(argv[i]);
        } else if (arg == "--virtual-display") {
            if_throw(i, "--virtual-display requires a display name");
            args.video = true;
            args.virtual_display = argv[i];
        } else if (arg == "--video-resolution") {
            if_throw(i, "--video-resolution requires a WxH value");
            args.video = true;
            args.video_resolution = argv[i];
        } else if (arg == "--display-backend") {
            if_throw(i, "--display-backend requires auto, xvfb, or xephyr");
            args.video = true;
            args.display_backend = parse_display_backend(argv[i]);
        } else if (arg == "--bridge-controller") {
            if_throw(i, "--bridge-controller requires a controller index");
            args.bridge_controller_index = static_cast<std::size_t>(std::stoul(argv[i]));
        } else if (arg == "--ignore-controller") {
            if_throw(i, "--ignore-controller requires a VID/PID pair");
            args.ignore_controller = argv[i];
        } else if (arg == "--retroarch-joypad-driver") {
            if_throw(i, "--retroarch-joypad-driver requires a driver name");
            args.retroarch_joypad_driver = argv[i];
        } else if (arg == "--save-root") {
            if_throw(i, "--save-root requires a path");
            args.save_root = argv[i];
        } else if (arg == "--username") {
            if_throw(i, "--username requires a name");
            args.username = argv[i];
        } else if (arg == "--virtual-joypad-index") {
            if_throw(i, "--virtual-joypad-index requires an index");
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

    if (args.clients == 0 || args.clients > MaxRemoteClients) {
        throw std::runtime_error("--clients must be between 1 and MaxRemoteClients");
    }

    if (args.save_root.empty()) {
        args.save_root = default_save_profile_root();
    }

    return args;
}

} // namespace archstreamer
