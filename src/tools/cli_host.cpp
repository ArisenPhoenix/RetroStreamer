#include "tools/cli.hpp"

#include "common/cli_common.hpp"
#include "host/console/save_profile.hpp"

#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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
    if (value == "virtualgl" || value == "vgl") {
        return VirtualDisplayBackend::VirtualGL;
    }
    if (value == "gamescope" || value == "gs") {
        return VirtualDisplayBackend::Gamescope;
    }

    throw std::runtime_error("--display-backend must be auto, xvfb, xephyr, virtualgl, or gamescope");
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

GraphicsApiPreference parse_graphics_api(std::string_view value) {
    if (value == "auto") {
        return GraphicsApiPreference::Auto;
    }
    if (value == "opengl" || value == "gl" || value == "ogl") {
        return GraphicsApiPreference::OpenGL;
    }
    if (value == "vulkan" || value == "vk") {
        return GraphicsApiPreference::Vulkan;
    }

    throw std::runtime_error("--renderer must be auto, opengl, or vulkan");
}

std::string trim_copy(std::string_view value) {
    auto begin = value.begin();
    auto end = value.end();
    while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) {
        ++begin;
    }
    while (begin != end && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
        --end;
    }
    return std::string(begin, end);
}

std::string normalize_config_key(std::string key) {
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
        if (ch == '_') {
            return '-';
        }
        return static_cast<char>(std::tolower(ch));
    });
    return key;
}

std::string compact_config_key(std::string key) {
    std::string compact;
    compact.reserve(key.size());
    for (const unsigned char ch : key) {
        if (ch == '-' || ch == '_' || ch == ' ' || ch == '\t') {
            continue;
        }
        compact.push_back(static_cast<char>(std::tolower(ch)));
    }
    return compact;
}

std::string lower_config_value(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool parse_config_bool(std::string_view value) {
    auto text = trim_copy(value);
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (text == "1" || text == "true" || text == "yes" || text == "on") {
        return true;
    }
    if (text == "0" || text == "false" || text == "no" || text == "off") {
        return false;
    }
    throw std::runtime_error("config boolean must be true/false, yes/no, on/off, or 1/0");
}

bool is_value_option(const std::string& key) {
    static const char* const kOptions[] = {
        "rom-root",
        "meta-root",
        "art-root",
        "mode",
        "players",
        "control-port",
        "input-port",
        "clients",
        "session-timeout",
        "client-timeout",
        "player-reconnect-timeout",
        "host-role",
        "video-dest",
        "video-port",
        "audio-port",
        "audio-source",
        "audio-backend",
        "virtual-display",
        "video-resolution",
        "display-backend",
        "renderer",
        "switch-resolution",
        "retroarch-resolution",
        "bridge-controller",
        "ignore-controller",
        "retroarch-joypad-driver",
        "save-root",
        "log-root",
        "username",
        "host-name",
        "virtual-joypad-index",
        "gpu",
        "render-gpu",
        "owner-gui-pid",
    };
    return std::find(std::begin(kOptions), std::end(kOptions), key) != std::end(kOptions);
}

void append_config_bool_option(std::vector<std::string>& args, const std::string& key, bool value) {
    if (key == "video") {
        args.push_back(value ? "--video" : "--no-video");
    } else if (key == "audio") {
        args.push_back(value ? "--audio" : "--no-audio");
    } else if (key == "allow-new-users" || key == "dry-run" || key == "pulse-input"
               || key == "verbose" || key == "host-local-watch" || key == "separate-render-gpu"
               || key == "list" || key == "list-gpus") {
        if (value) {
            args.push_back("--" + key);
        }
    } else {
        throw std::runtime_error("unknown host_runner config key: " + key);
    }
}

std::string gui_settings_key_to_config_key(
    const std::string& section,
    const std::string& key) {
    const auto normalized_section = compact_config_key(section);
    const auto normalized_key = compact_config_key(key);

    if (normalized_section == "host") {
        if (normalized_key == "romroot") return "rom-root";
        if (normalized_key == "metaroot") return "meta-root";
        if (normalized_key == "saveroot") return "save-root";
        if (normalized_key == "logroot") return "log-root";
        if (normalized_key == "mode") return "mode";
        if (normalized_key == "maxclients") return "clients";
        if (normalized_key == "controlport") return "control-port";
        if (normalized_key == "inputport") return "input-port";
        if (normalized_key == "videoport") return "video-port";
        if (normalized_key == "audioport") return "audio-port";
        if (normalized_key == "sessiontimeoutseconds") return "session-timeout";
        if (normalized_key == "playerreconnecttimeoutseconds") {
            return "player-reconnect-timeout";
        }
        if (normalized_key == "role") return "host-role";
        if (normalized_key == "captureresolution") return "video-resolution";
        if (normalized_key == "streamvideo") return "video";
        if (normalized_key == "streamaudio") return "audio";
        if (normalized_key == "watchlocally") return "host-local-watch";
        if (normalized_key == "allownewusers") return "allow-new-users";
        return {};
    }

    if (normalized_section == "paths") {
        if (normalized_key == "artroot") return "art-root";
        return {};
    }

    if (normalized_section == "graphics") {
        if (normalized_key == "encodegpuid") return "gpu";
        if (normalized_key == "rendergpuid") return "render-gpu";
        if (normalized_key == "separaterendergpu") return "separate-render-gpu";
        if (normalized_key == "renderer") return "renderer";
        if (normalized_key == "switchresolutionscale") return "switch-resolution";
        if (normalized_key == "retroarchresolutionscale") return "retroarch-resolution";
        return {};
    }

    if (normalized_section == "profile") {
        if (normalized_key == "username") return "username";
        if (normalized_key == "hostname") return "host-name";
        return {};
    }

    return {};
}

void append_config_option(
    std::vector<std::string>& args,
    const std::string& key,
    std::string value,
    const std::filesystem::path& path,
    std::size_t line_number,
    bool strict_unknown,
    bool strict_empty) {
    if (key.empty()) {
        return;
    }
    if (key == "mode" || key == "host-role" || key == "renderer" || key == "display-backend"
        || key == "audio-backend") {
        value = lower_config_value(value);
    }
    if (is_value_option(key)) {
        if (value.empty()) {
            if (!strict_empty) {
                return;
            }
            throw std::runtime_error(
                "invalid host_runner config line " + std::to_string(line_number)
                + " in " + path.string() + ": empty value for " + key);
        }
        args.push_back("--" + key);
        args.push_back(std::move(value));
        return;
    }
    try {
        append_config_bool_option(args, key, parse_config_bool(value));
    } catch (const std::exception& error) {
        if (!strict_unknown && std::string(error.what()).find("unknown host_runner config key") == 0) {
            return;
        }
        throw std::runtime_error(
            "invalid host_runner config line " + std::to_string(line_number)
            + " in " + path.string() + ": " + error.what());
    }
}

std::vector<std::string> host_config_file_to_argv(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("failed to open host_runner config: " + path.string());
    }

    std::vector<std::string> args;
    std::string line;
    std::string section;
    std::size_t line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        const auto comment = line.find('#');
        if (comment != std::string::npos) {
            line.resize(comment);
        }
        line = trim_copy(line);
        if (line.empty()) {
            continue;
        }
        if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
            section = trim_copy(std::string_view(line).substr(1, line.size() - 2));
            continue;
        }
        const auto separator = line.find_first_of("=:");
        if (separator == std::string::npos) {
            throw std::runtime_error(
                "invalid host_runner config line " + std::to_string(line_number)
                + " in " + path.string() + ": expected key=value");
        }

        auto key = normalize_config_key(trim_copy(std::string_view(line).substr(0, separator)));
        auto value = trim_copy(std::string_view(line).substr(separator + 1));
        if (key.empty()) {
            throw std::runtime_error(
                "invalid host_runner config line " + std::to_string(line_number)
                + " in " + path.string() + ": empty key");
        }

        if (!section.empty()) {
            append_config_option(
                args,
                gui_settings_key_to_config_key(section, key),
                std::move(value),
                path,
                line_number,
                false,
                false);
            continue;
        }
        append_config_option(args, key, std::move(value), path, line_number, true, true);
    }
    return args;
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
        << "  --config <path>     Load host defaults from key=value config or GUI settings.\n"
        << "  --rom-root <path>   ROM root to scan (required; no built-in machine path).\n"
        << "                      Typical layout: <Gaming>/ROMS/Games\n"
        << "  --meta-root <path>  Metadata root. Default: sibling Meta directory next to ROM root.\n"
        << "  --art-root <path>   Artwork root served to clients. Default: sibling Art next to ROM root.\n"
        << "  --mode <mode>       singleplayer or multiplayer. Default: singleplayer\n"
        << "  --players <count>   Number of virtual pads to plug. Default: 1\n"
        << "  --list              List games and exit.\n"
        << "  --list-gpus         List encode/render GPUs (id + name) and exit.\n"
        << "  --dry-run           Print selected launch config without launching.\n"
        << "  --pulse-input       Tap A on port 1 shortly after launch.\n"
        << "  --verbose           Pass --verbose to RetroArch.\n"
        << "  --control-port <port>\n"
        << "                      Wait for TCP session clients before launch.\n"
        << "  --input-port <port> Receive UDP ControllerInput packets on this port.\n"
        << "  --clients <count>   Max concurrent singleplayer sessions (also MP lobby size).\n"
        << "                      1–" << static_cast<int>(MaxConcurrentSessionSlots)
        << ". Omitted defaults to " << static_cast<int>(MaxConcurrentSessionSlots) << ".\n"
        << "  --session-timeout <seconds>\n"
        << "                      Maximum time to wait for enough players. Default: 30\n"
        << "  --client-timeout <seconds>\n"
        << "                      Maximum time without client heartbeat during play. Default: 20\n"
        << "  --player-reconnect-timeout <seconds>\n"
        << "                      Time to reserve disconnected player seats. Default: 60\n"
        << "  --host-role <role>  player or viewer for the host. Default: viewer\n"
        << "                      player requires --bridge-controller. Does not control streaming.\n"
        << "  --video             Capture RetroArch from a virtual display and stream RTP/H.264 (default on).\n"
        << "  --no-video          Disable video streaming.\n"
        << "  --video-dest <ip>   Override video destination IP for all clients.\n"
        << "  --video-port <port> Base destination UDP video port. Default: 5004\n"
        << "  --audio             Capture host audio and stream RTP/Opus (default on).\n"
        << "  --no-audio          Disable audio streaming.\n"
        << "  --audio-port <port> Base destination UDP audio port. Default: 6004\n"
        << "  --host-local-watch  Add a 127.0.0.1 loopback stream for host-side viewing.\n"
        << "  --audio-source <source>\n"
        << "                      Pulse/PipeWire source to capture. Default: archstreamer.monitor\n"
        << "                      (dedicated null sink so host speakers stay quiet).\n"
        << "  --audio-backend <pulse|pipewire>\n"
        << "                      Audio capture backend. Default: pulse\n"
        << "  --virtual-display <display>\n"
        << "                      X display for virtual RetroArch output. Default: :99\n"
        << "  --video-resolution <WxH>\n"
        << "                      Virtual display resolution. Default: 1920x1080\n"
        << "  --display-backend <auto|xvfb|xephyr|virtualgl|gamescope>\n"
        << "                      Virtual display backend. Default: auto\n"
        << "                      (Switch/standalone defaults to gamescope when streaming).\n"
        << "  --renderer <auto|opengl|vulkan>\n"
        << "                      Preferred graphics API for Switch standalone. Default: auto\n"
        << "                      (Vulkan on gamescope, OpenGL on VirtualGL). Ignored for RetroArch.\n"
        << "  --bridge-controller <index>\n"
        << "                      Use a local SDL2 controller as host player 1.\n"
        << "  --ignore-controller <vid/pid>\n"
        << "                      Hide a physical controller from RetroArch SDL2, for example 0x054c/0x09cc.\n"
        << "  --retroarch-joypad-driver <driver>\n"
        << "                      RetroArch joypad driver (udev|sdl2|xinput). "
           "Default: platform ("
#if defined(_WIN32)
           "xinput"
#else
           "udev"
#endif
           "; preferred).\n"
        << "                      sdl2 can stall heavy cores like LRPS2).\n"
        << "  --save-root <path>  Base save profile directory. Default: ~/.local/share/archstreamer/saves\n"
        << "  --log-root <path>   Directory for host_<control-port>.log when host_runner owns logging.\n"
        << "                      Default: no file redirect; wrappers may use their own fallback.\n"
        << "  --allow-new-users   Allow ClientHello to create new save profiles (off by default).\n"
        << "  --username <name>   Save profile username. Default: $USER or local.\n"
        << "  --host-name <name>  Stable host identity name. Default: --username.\n"
        << "  --virtual-joypad-index <index>\n"
        << "                      RetroArch joypad index for the virtual pad. Default: 1 with bridge.\n"
        << "  --gpu <auto|id|name>\n"
        << "                      Encode GPU (nvenc). Also used for game render unless\n"
        << "                      --separate-render-gpu is set. Auto = most performant.\n"
        << "                      Ids from nvidia-smi as nvidia:0, nvidia:1, …\n"
        << "  --separate-render-gpu\n"
        << "                      Use --render-gpu for RetroArch/Switch (PRIME); --gpu stays encode.\n"
        << "  --render-gpu <auto|id|name>\n"
        << "                      Game render GPU when --separate-render-gpu is set.\n"
        << "  --switch-resolution <1-6>\n"
        << "                      Switch standalone internal resolution (1x…6x native).\n"
        << "                      Applied to Ryujinx/Yuzu. Default: 1. Ignored for RetroArch.\n"
        << "  --retroarch-resolution <1-6>\n"
        << "                      RetroArch internal resolution multiplier (1x…6x).\n"
        << "                      Applied to known cores' .opt on launch. Default: 1.\n"
        << "  --owner-gui-pid <pid>\n"
        << "                      Internal: stop when this supervising GUI process exits.\n";
}

HostAppConfig HostRunnerCli::parse(int argc, char** argv) const {
    HostAppConfig args;
    std::vector<std::string> merged_args;
    std::vector<std::string> cli_args;
    merged_args.emplace_back(argc > 0 && argv[0] != nullptr ? argv[0] : "host_runner");
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] != nullptr ? argv[i] : "";
        std::string value;
        if (arg == "--config") {
            if (++i >= argc || argv[i] == nullptr) {
                throw std::runtime_error("--config requires a path");
            }
            const auto config_args = host_config_file_to_argv(argv[i]);
            merged_args.insert(merged_args.end(), config_args.begin(), config_args.end());
            continue;
        }
        if (arg.rfind("--config=", 0) == 0) {
            const auto config_args = host_config_file_to_argv(arg.substr(std::string("--config=").size()));
            merged_args.insert(merged_args.end(), config_args.begin(), config_args.end());
            continue;
        }
        cli_args.push_back(arg);
    }
    merged_args.insert(merged_args.end(), cli_args.begin(), cli_args.end());

    auto if_throw = [&merged_args](std::size_t& i, std::string arg) -> bool {
        if (++i >= merged_args.size()) {
            throw std::runtime_error(arg);
        }
        return false;
    };

    for (std::size_t i = 1; i < merged_args.size(); ++i) {
        const std::string arg = merged_args[i];
        
        if (arg == "--list") {
            args.list = true;
        } else if (arg == "--list-gpus") {
            args.list_gpus = true;
        } else if (arg == "--dry-run") {
            args.dry_run = true;
        } else if (arg == "--pulse-input") {
            args.pulse_input = true;
        } else if (arg == "--verbose") {
            args.verbose = true;
        } else if (arg == "--host-local-watch") {
            args.host_local_watch = true;
        } else if (arg == "--video") {
            args.video = true;
        } else if (arg == "--no-video") {
            args.video = false;
        } else if (arg == "--audio") {
            args.audio = true;
        } else if (arg == "--no-audio") {
            args.audio = false;
        } else if (arg == "--rom-root") {
            if_throw(i, "--rom-root requires a path");
            args.rom_root = merged_args[i];
        } else if (arg == "--meta-root") {
            if_throw(i, "--meta-root requires a path");
            args.meta_root = merged_args[i];
        } else if (arg == "--art-root") {
            if_throw(i, "--art-root requires a path");
            args.art_root = merged_args[i];
        } else if (arg == "--mode") {
            if_throw(i, "--mode requires singleplayer or multiplayer");
            args.session_mode = parse_session_mode(merged_args[i]);
        } else if (arg == "--players") {
            if_throw(i, "--players requires a count");
           
            const auto count = std::stoul(merged_args[i]);
            if (count == 0 || count > MaxRetroArchPorts) {
                throw std::runtime_error("--players must be between 1 and MaxRetroArchPorts");
            }
            args.players = static_cast<std::uint8_t>(count);
        } else if (arg == "--control-port") {
            if_throw(i, "--control-port requires a port");
            args.control_port = static_cast<std::uint16_t>(std::stoul(merged_args[i]));
        } else if (arg == "--input-port") {
            if_throw(i, "--input-port requires a port");
            args.input_port = static_cast<std::uint16_t>(std::stoul(merged_args[i]));
        } else if (arg == "--clients") {
            if_throw(i, "--clients requires a count");
            args.clients = static_cast<std::uint8_t>(std::stoul(merged_args[i]));
        } else if (arg == "--session-timeout") {
            if_throw(i, "--session-timeout requires seconds");
            args.session_timeout_seconds = static_cast<std::uint16_t>(std::stoul(merged_args[i]));
        } else if (arg == "--client-timeout") {
            if_throw(i, "--client-timeout requires seconds");
            args.client_timeout_seconds = static_cast<std::uint16_t>(std::stoul(merged_args[i]));
            if (args.client_timeout_seconds == 0) {
                throw std::runtime_error("--client-timeout must be greater than zero");
            }
        } else if (arg == "--player-reconnect-timeout") {
            if_throw(i, "--player-reconnect-timeout requires seconds");
            args.player_reconnect_timeout_seconds = static_cast<std::uint16_t>(std::stoul(merged_args[i]));
            if (args.player_reconnect_timeout_seconds == 0) {
                throw std::runtime_error("--player-reconnect-timeout must be greater than zero");
            }
        } else if (arg == "--host-role") {
            if_throw(i, "--host-role requires player or viewer");
            args.host_role = parse_participant_role(merged_args[i]);
        } else if (arg == "--video-dest") {
            if_throw(i, "--video-dest requires an IP address");
            args.video = true;
            args.video_destination = merged_args[i];
            args.video_destination_explicit = true;
        } else if (arg == "--video-port") {
            if_throw(i, "--video-port requires a port");
            args.video = true;
            args.video_port = static_cast<std::uint16_t>(std::stoul(merged_args[i]));
        } else if (arg == "--audio-port") {
            if_throw(i, "--audio-port requires a port");
            args.audio = true;
            args.audio_port = static_cast<std::uint16_t>(std::stoul(merged_args[i]));
        } else if (arg == "--audio-source") {
            if_throw(i, "--audio-source requires a source name");
            args.audio = true;
            args.audio_source = merged_args[i];
        } else if (arg == "--audio-backend") {
            if_throw(i, "--audio-backend requires pulse or pipewire");
            args.audio = true;
            args.audio_backend = parse_audio_backend(merged_args[i]);
        } else if (arg == "--virtual-display") {
            if_throw(i, "--virtual-display requires a display name");
            args.video = true;
            args.virtual_display = merged_args[i];
        } else if (arg == "--video-resolution") {
            if_throw(i, "--video-resolution requires a WxH value");
            args.video = true;
            args.video_resolution = merged_args[i];
        } else if (arg == "--display-backend") {
            if_throw(i, "--display-backend requires auto, xvfb, xephyr, virtualgl, or gamescope");
            args.video = true;
            args.display_backend = parse_display_backend(merged_args[i]);
        } else if (arg == "--renderer") {
            if_throw(i, "--renderer requires auto, opengl, or vulkan");
            args.graphics_api = parse_graphics_api(merged_args[i]);
        } else if (arg == "--switch-resolution") {
            if_throw(i, "--switch-resolution requires an integer 1-6");
            args.resolution.switch_scale = std::clamp(std::stoi(merged_args[i]), 1, 6);
        } else if (arg == "--retroarch-resolution") {
            if_throw(i, "--retroarch-resolution requires an integer 1-6");
            args.resolution.retroarch_scale = std::clamp(std::stoi(merged_args[i]), 1, 6);
        } else if (arg == "--bridge-controller") {
            if_throw(i, "--bridge-controller requires a controller index");
            args.bridge_controller_index = static_cast<std::size_t>(std::stoul(merged_args[i]));
        } else if (arg == "--ignore-controller") {
            if_throw(i, "--ignore-controller requires a VID/PID pair");
            args.ignore_controller = merged_args[i];
        } else if (arg == "--retroarch-joypad-driver") {
            if_throw(i, "--retroarch-joypad-driver requires a driver name");
            args.retroarch_joypad_driver = merged_args[i];
        } else if (arg == "--save-root") {
            if_throw(i, "--save-root requires a path");
            args.save_root = merged_args[i];
        } else if (arg == "--log-root") {
            if_throw(i, "--log-root requires a path");
            args.log_root = merged_args[i];
        } else if (arg == "--allow-new-users") {
            args.allow_new_users = true;
        } else if (arg == "--username") {
            if_throw(i, "--username requires a name");
            args.username = merged_args[i];
        } else if (arg == "--host-name") {
            if_throw(i, "--host-name requires a name");
            args.host_name = merged_args[i];
        } else if (arg == "--virtual-joypad-index") {
            if_throw(i, "--virtual-joypad-index requires an index");
            args.virtual_joypad_index = static_cast<std::size_t>(std::stoul(merged_args[i]));
        } else if (arg == "--gpu") {
            if_throw(i, "--gpu requires auto, a device id (nvidia:0), or a name substring");
            args.encode_gpu = merged_args[i];
        } else if (arg == "--separate-render-gpu") {
            args.separate_render_gpu = true;
        } else if (arg == "--render-gpu") {
            if_throw(i, "--render-gpu requires auto, a device id (nvidia:0), or a name substring");
            args.render_gpu = merged_args[i];
            args.separate_render_gpu = true;
        } else if (arg == "--owner-gui-pid") {
            if_throw(i, "--owner-gui-pid requires a pid");
            args.owner_gui_pid = std::stoi(merged_args[i]);
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(0);
        } else if (arg == "--") {
            break;
        } else if (!arg.empty() && arg.front() == '-') {
            if (i + 1 < merged_args.size() && !merged_args[i + 1].empty()
                && merged_args[i + 1].front() != '-') {
                ++i;
            }
        } else if (!args.selector.has_value()) {
            args.selector = arg;
        } else {
            throw std::runtime_error("unexpected extra argument: " + arg);
        }
    }

    if (args.clients == 0 || args.clients > MaxConcurrentSessionSlots) {
        throw std::runtime_error(
            "--clients must be between 1 and "
            + std::to_string(static_cast<int>(MaxConcurrentSessionSlots)));
    }

    if (args.save_root.empty()) {
        args.save_root = default_save_profile_root();
    }

    return args;
}

} // namespace archstreamer
