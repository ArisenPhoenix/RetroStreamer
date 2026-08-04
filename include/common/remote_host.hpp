#pragma once

#include <cctype>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace archstreamer {

/** Default control / input / media ports for a remote host_runner instance. */
constexpr std::uint16_t RemoteDefaultControlPort = 45555;
constexpr std::uint16_t RemoteDefaultInputPort = 45454;
constexpr std::uint16_t RemoteDefaultVideoPort = 5004;
constexpr std::uint16_t RemoteDefaultAudioPort = 6004;
constexpr int RemoteDefaultVirtualDisplay = 99;

/**
 * Port block for remote overflow instance index n (n >= 0).
 * Instance 0 uses the base ports; n >= 1 offsets by 10*n.
 */
struct RemoteHostPortBlock {
    std::uint16_t control_port = RemoteDefaultControlPort;
    std::uint16_t input_port = RemoteDefaultInputPort;
    std::uint16_t video_port = RemoteDefaultVideoPort;
    std::uint16_t audio_port = RemoteDefaultAudioPort;
    int virtual_display = RemoteDefaultVirtualDisplay;
    int instance_index = 0;
};

inline RemoteHostPortBlock remote_host_port_block(
    int instance_index,
    std::uint16_t base_control = RemoteDefaultControlPort,
    std::uint16_t base_input = RemoteDefaultInputPort,
    std::uint16_t base_video = RemoteDefaultVideoPort,
    std::uint16_t base_audio = RemoteDefaultAudioPort,
    int base_display = RemoteDefaultVirtualDisplay) {
    const auto n = instance_index < 0 ? 0 : instance_index;
    const auto offset = static_cast<std::uint16_t>(10 * n);
    RemoteHostPortBlock block;
    block.instance_index = n;
    block.control_port = static_cast<std::uint16_t>(base_control + offset);
    block.input_port = static_cast<std::uint16_t>(base_input + offset);
    block.video_port = static_cast<std::uint16_t>(base_video + offset);
    block.audio_port = static_cast<std::uint16_t>(base_audio + offset);
    block.virtual_display = base_display + n;
    return block;
}

inline std::string remote_shell_single_quote(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('\'');
    for (char ch : value) {
        if (ch == '\'') {
            escaped += "'\\''";
        } else {
            escaped.push_back(ch);
        }
    }
    escaped.push_back('\'');
    return escaped;
}

/** PID file name under the remote install directory. */
inline std::string remote_host_pid_filename(std::uint16_t control_port) {
    return ".archstreamer_remote_" + std::to_string(control_port) + ".pid";
}

inline std::string remote_host_pid_path(const std::string& directory, std::uint16_t control_port) {
    if (directory.empty()) {
        return remote_host_pid_filename(control_port);
    }
    if (directory.back() == '/') {
        return directory + remote_host_pid_filename(control_port);
    }
    return directory + "/" + remote_host_pid_filename(control_port);
}

inline std::string remote_host_log_path(const std::string& directory, std::uint16_t control_port) {
    const auto name = "host_" + std::to_string(control_port) + ".log";
    if (directory.empty()) {
        return name;
    }
    if (directory.back() == '/') {
        return directory + name;
    }
    return directory + "/" + name;
}

/**
 * Resolve host_runner to an absolute remote path when possible.
 * Relative binaries (./host_runner) must be joined to the remote directory because
 * start no longer cds into that directory.
 */
inline std::string remote_host_resolve_binary(
    const std::string& directory,
    const std::string& binary) {
    auto trimmed = binary;
    while (!trimmed.empty() && (trimmed.back() == '/' || trimmed.back() == '\\')) {
        trimmed.pop_back();
    }
    const auto dir = [&] {
        auto d = directory;
        while (!d.empty() && (d.back() == '/' || d.back() == '\\')) {
            d.pop_back();
        }
        return d;
    }();

    auto join_dir = [&](const std::string& leaf) {
        if (dir.empty()) {
            return leaf;
        }
        return dir + "/" + leaf;
    };

    if (trimmed.empty() || trimmed == "." || trimmed == "./") {
        return join_dir("host_runner");
    }

    // Absolute path already.
    if (!trimmed.empty() && trimmed.front() == '/') {
        const auto slash = trimmed.find_last_of('/');
        const auto leaf = slash == std::string::npos ? trimmed : trimmed.substr(slash + 1);
        if (leaf == "build" || leaf == "bin" || leaf == "Debug" || leaf == "Release") {
            return trimmed + "/host_runner";
        }
        return trimmed;
    }

    // Strip leading ./
    if (trimmed.size() >= 2 && trimmed[0] == '.' && trimmed[1] == '/') {
        trimmed = trimmed.substr(2);
    }

    const auto slash = trimmed.find_last_of("/\\");
    const auto leaf = slash == std::string::npos ? trimmed : trimmed.substr(slash + 1);
    if (leaf == "build" || leaf == "bin" || leaf == "Debug" || leaf == "Release") {
        return join_dir(trimmed + "/host_runner");
    }
    return join_dir(trimmed);
}

/**
 * Start host_runner without cd — log/PID use absolute paths under directory.
 * directory/binary/rom_root are unquoted filesystem paths.
 * Verifies the binary exists and the process is still alive shortly after spawn
 * so "nohup: failed to run command" surfaces as an SSH failure instead of a
 * silent success + unanswered control port.
 */
inline std::string remote_host_start_shell(
    const std::string& directory,
    const std::string& binary,
    const std::string& rom_root,
    const RemoteHostPortBlock& ports,
    const std::string& encode_gpu = {}) {
    const auto resolved = remote_host_resolve_binary(directory, binary);
    const auto qbin = remote_shell_single_quote(resolved);
    const auto qrom = remote_shell_single_quote(rom_root);
    const auto qdir = remote_shell_single_quote([&] {
        auto d = directory;
        while (!d.empty() && (d.back() == '/' || d.back() == '\\')) {
            d.pop_back();
        }
        return d.empty() ? std::string(".") : d;
    }());
    const auto qlog = remote_shell_single_quote(remote_host_log_path(directory, ports.control_port));
    const auto qpid = remote_shell_single_quote(remote_host_pid_path(directory, ports.control_port));
    std::string gpu_args;
    if (!encode_gpu.empty()) {
        gpu_args = " --gpu " + remote_shell_single_quote(encode_gpu);
    }
    return std::string("set -e; ")
        + "mkdir -p "
        + qdir
        + "; "
        + "if [ ! -x "
        + qbin
        + " ]; then "
        + "echo \"host_runner not found or not executable: \" "
        + qbin
        + " >&2; exit 127; fi; "
        + "nohup "
        + qbin
        + " --rom-root "
        + qrom
        + " --control-port "
        + std::to_string(ports.control_port)
        + " --input-port "
        + std::to_string(ports.input_port)
        + " --video-port "
        + std::to_string(ports.video_port)
        + " --audio-port "
        + std::to_string(ports.audio_port)
        + " --virtual-display :"
        + std::to_string(ports.virtual_display)
        + " --clients 2 --allow-new-users"
        + gpu_args
        + " > "
        + qlog
        + " 2>&1 & "
        + "pid=$!; "
        + "echo \"$pid\" > "
        + qpid
        + "; "
        + "sleep 0.7; "
        + "if ! kill -0 \"$pid\" 2>/dev/null; then "
        + "echo \"host_runner exited immediately (pid $pid). Log:\" >&2; "
        + "cat "
        + qlog
        + " >&2 || true; "
        + "rm -f "
        + qpid
        + "; exit 1; fi";
}

/**
 * Stop by control-port match (does not require the install directory).
 * If directory is set, also SIGTERM via the absolute PID file from Ensure.
 */
inline std::string remote_host_stop_shell(
    std::uint16_t control_port,
    const std::string& directory = {}) {
    std::string cmd;
    if (!directory.empty()) {
        const auto qpid = remote_shell_single_quote(remote_host_pid_path(directory, control_port));
        cmd += "if [ -f "
            + qpid
            + " ]; then"
            + " pid=$(cat "
            + qpid
            + ");"
            + " kill \"$pid\" 2>/dev/null;"
            + " sleep 1;"
            + " kill -0 \"$pid\" 2>/dev/null && kill -9 \"$pid\" 2>/dev/null;"
            + " rm -f "
            + qpid
            + "; fi; ";
    }
    // Match host_runner argv for this control port; [h] avoids matching pkill itself.
    cmd += "pkill -f '[h]ost_runner.*--control-port[= ]*"
        + std::to_string(control_port)
        + "' >/dev/null 2>&1 || true; "
        + "sleep 1; "
        + "pkill -9 -f '[h]ost_runner.*--control-port[= ]*"
        + std::to_string(control_port)
        + "' >/dev/null 2>&1 || true";
    return cmd;
}

/** True when slot fields are present and the lobby reports no free slots. */
inline bool remote_host_lobby_full(const std::optional<std::uint8_t>& active_slots,
                                   const std::optional<std::uint8_t>& max_slots) {
    if (!active_slots.has_value() || !max_slots.has_value()) {
        return false;
    }
    if (*max_slots == 0) {
        return false;
    }
    return *active_slots >= *max_slots;
}

/**
 * Fuzzy GPU preference match for Remote Ensure Host.
 * Compares the user-typed preference (e.g. "3060", "nvidia 3060", "nvidia:1") against
 * the remote process --gpu value. Empty want = any. Empty reported with non-empty want
 * = no match (instance was started without a GPU pin / unknown).
 */
inline bool remote_host_gpu_preference_matches(
    const std::string& want,
    const std::string& reported) {
    if (want.empty()) {
        return true;
    }
    auto normalize = [](std::string s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            if (c == ' ' || c == '\t' || c == '_' || c == '-' || c == ':') {
                continue;
            }
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return out;
    };
    const auto needle = normalize(want);
    const auto hay = normalize(reported);
    if (needle.empty()) {
        return true;
    }
    if (hay.empty() || hay == "auto") {
        return false;
    }
    return hay == needle || hay.find(needle) != std::string::npos
        || needle.find(hay) != std::string::npos;
}

/**
 * Fuzzy match a remoting preference against a GPU option list (same rules as
 * resolve_render_gpu / the Settings GPU combo). Empty want → nullopt (caller uses default).
 */
inline std::optional<std::pair<std::string, std::string>> remote_host_match_gpu_option(
    const std::vector<std::pair<std::string, std::string>>& options, // id, name
    const std::string& want) {
    if (want.empty() || want == "auto") {
        return std::nullopt;
    }
    auto normalize = [](std::string value) {
        for (char& c : value) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (c == ':' || c == '_' || c == '-' || c == '/') {
                c = ' ';
            }
        }
        return value;
    };
    auto matches = [&](const std::string& id, const std::string& name) {
        if (id == want) {
            return true;
        }
        const auto needle = normalize(want);
        const auto hay = normalize(id + " " + name);
        if (hay.find(needle) != std::string::npos) {
            return true;
        }
        std::string token;
        std::istringstream tokens(needle);
        bool any = false;
        while (tokens >> token) {
            if (token.empty()) {
                continue;
            }
            any = true;
            if (hay.find(token) == std::string::npos) {
                return false;
            }
        }
        return any;
    };
    for (const auto& [id, name] : options) {
        if (id == want) {
            return std::pair{id, name};
        }
    }
    for (const auto& [id, name] : options) {
        if (matches(id, name)) {
            return std::pair{id, name};
        }
    }
    return std::nullopt;
}

/**
 * Print encode/render GPUs via the remote host_runner binary (same list as the GUI).
 * Output lines: id<TAB>name…
 */
inline std::string remote_host_list_gpus_shell(
    const std::string& directory,
    const std::string& binary) {
    const auto resolved = remote_host_resolve_binary(directory, binary);
    const auto qbin = remote_shell_single_quote(resolved);
    return std::string("set -e; ")
        + "if [ ! -x "
        + qbin
        + " ]; then "
        + "echo \"host_runner not found or not executable: \" "
        + qbin
        + " >&2; exit 127; fi; "
        + qbin
        + " --list-gpus";
}

/**
 * Print the --gpu value from the host_runner argv for this control port (or empty).
 */
inline std::string remote_host_encode_gpu_query_shell(std::uint16_t control_port) {
    const auto port = std::to_string(control_port);
    return std::string(
               "found=\"\"; "
               "for f in /proc/[0-9]*/cmdline; do "
               "cmd=$(tr '\\0' ' ' < \"$f\" 2>/dev/null) || continue; "
               "case \"$cmd\" in *host_runner*) ;; *) continue ;; esac; "
               "echo \"$cmd\" | grep -Eq -- '--control-port[= ]*")
        + port
        + "([[:space:]]|$)' || continue; "
               "found=$(echo \"$cmd\" | sed -n 's/.*--gpu[= ]\\([^[:space:]]*\\).*/\\1/p'); "
               "break; "
               "done; "
               "printf '%s\\n' \"$found\"";
}

} // namespace archstreamer
