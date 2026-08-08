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
 * Resolve an optional remote start-script path (absolute, or relative to directory).
 * Empty script → empty result (caller uses host_runner Path A).
 */
inline std::string remote_host_resolve_start_script(
    const std::string& directory,
    const std::string& start_script) {
    auto trimmed = start_script;
    while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) {
        trimmed.erase(trimmed.begin());
    }
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'
                                || trimmed.back() == '/' || trimmed.back() == '\\')) {
        trimmed.pop_back();
    }
    if (trimmed.empty()) {
        return {};
    }
    if (!trimmed.empty() && trimmed.front() == '/') {
        return trimmed;
    }
    if (trimmed.size() >= 2 && trimmed[0] == '.' && trimmed[1] == '/') {
        trimmed = trimmed.substr(2);
    }
    auto dir = directory;
    while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) {
        dir.pop_back();
    }
    if (dir.empty()) {
        return trimmed;
    }
    return dir + "/" + trimmed;
}

/**
 * Start host without cd — log/PID use absolute paths under directory.
 * directory/binary/rom_root/start_script are unquoted filesystem paths.
 *
 * Path A (start_script empty): nohup host_runner with rom-root, ports, clients, GPU.
 * Path B (start_script set): nohup that script with ports + GPU only; the script owns
 * ROM root / host_runner location / setup. Verifies the launched process is still alive
 * shortly after spawn so spawn failures surface as SSH errors.
 */
inline std::string remote_host_start_shell(
    const std::string& directory,
    const std::string& binary,
    const std::string& rom_root,
    const RemoteHostPortBlock& ports,
    const std::string& encode_gpu = {},
    const std::string& start_script = {},
    std::uint16_t player_reconnect_timeout_seconds = 60) {
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
    const auto port_args = std::string(" --control-port ")
        + std::to_string(ports.control_port)
        + " --input-port "
        + std::to_string(ports.input_port)
        + " --video-port "
        + std::to_string(ports.video_port)
        + " --audio-port "
        + std::to_string(ports.audio_port)
        + " --virtual-display :"
        + std::to_string(ports.virtual_display);

    const auto resolved_script = remote_host_resolve_start_script(directory, start_script);
    const bool use_script = !resolved_script.empty();
    const std::string launch_target = use_script
        ? resolved_script
        : remote_host_resolve_binary(directory, binary);
    const auto qlaunch = remote_shell_single_quote(launch_target);
    const char* missing_label = use_script ? "start script" : "host_runner";
    const char* exit_label = use_script ? "start script" : "host_runner";

    std::string launch_args = port_args + gpu_args;
    if (!use_script) {
        launch_args = std::string(" --rom-root ")
            + remote_shell_single_quote(rom_root)
            + port_args
            + " --clients 2 --allow-new-users"
            + " --player-reconnect-timeout "
            + std::to_string(player_reconnect_timeout_seconds)
            + gpu_args;
    }

    return std::string("set -e; ")
        + "mkdir -p "
        + qdir
        + "; "
        + "if [ ! -x "
        + qlaunch
        + " ]; then "
        + "echo \""
        + missing_label
        + " not found or not executable: \" "
        + qlaunch
        + " >&2; exit 127; fi; "
        + "nohup "
        + qlaunch
        + launch_args
        + " > "
        + qlog
        + " 2>&1 & "
        + "pid=$!; "
        + "echo \"$pid\" > "
        + qpid
        + "; "
        + "sleep 0.7; "
        + "if ! kill -0 \"$pid\" 2>/dev/null; then "
        + "echo \""
        + exit_label
        + " exited immediately (pid $pid). Log:\" >&2; "
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

/** Default remote saves root (shell expression; expands on the remote). */
inline std::string remote_host_default_saves_root_expr() {
    return "${XDG_DATA_HOME:-$HOME/.local/share}/archstreamer/saves";
}

/**
 * One Users-style presence row for Remote admin (Active session or Connected client).
 * kind: "active" | "connected"
 */
struct RemotePresenceRow {
    std::string kind;
    std::string username;
    std::uint32_t client_id = 0;
    int slot_index = -1;
    std::string phase;
    std::string display_name;
    std::string game_id;
    bool seated = false;
};

inline std::string remote_host_default_cadence_db_expr() {
    return "${XDG_DATA_HOME:-$HOME/.local/share}/archstreamer/cadence/cadence.sqlite";
}

/**
 * List Active sessions + Connected clients from cadence SQL on the remote host.
 * Output lines (TAB-separated):
 *   A\tusername\tslot\tdisplay\tgame_id
 *   C\tusername\tclient_id\tslot\tphase\tseated
 * Empty db_path → default XDG cadence.sqlite on the remote.
 */
inline std::string remote_host_list_presence_shell(const std::string& db_path = {}) {
    // Tab via printf, not $'\t': ssh runs the remote login shell, and dash does not expand
    // the $'…' form, so the columns would arrive glued together by a literal "$\t" and
    // parse as nothing — silently, since stderr is dropped.
    const std::string db_arg = db_path.empty()
        ? ("\"" + remote_host_default_cadence_db_expr() + "\"")
        : remote_shell_single_quote(db_path);
    std::string cmd;
    cmd += "DB=";
    cmd += db_arg;
    cmd += "; if [ ! -f \"$DB\" ]; then exit 0; fi; TAB=$(printf '\\t'); ";
    cmd += "sqlite3 -separator \"$TAB\" \"$DB\" "
           "\"SELECT 'A', username, slot, "
           "CASE WHEN game_key!='' THEN game_key ELSE '' END, game_key "
           "FROM sessions WHERE ended_at=0 AND username!='' "
           "ORDER BY started_at DESC;\" 2>/dev/null; ";
    cmd += "sqlite3 -separator \"$TAB\" \"$DB\" "
           "\"SELECT 'C', username, client_id, slot, "
           "CASE WHEN phase!='' THEN phase ELSE "
           "CASE WHEN slot<0 THEN 'lobby' ELSE 'session' END END, "
           "CASE WHEN seated!=0 THEN 1 ELSE 0 END "
           "FROM connections WHERE disconnected_at=0 AND username!='' AND client_id!=0 "
           "ORDER BY connected_at DESC;\" 2>/dev/null";
    return cmd;
}

inline std::vector<RemotePresenceRow> remote_host_parse_presence_output(const std::string& output) {
    std::vector<RemotePresenceRow> out;
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        std::vector<std::string> cols;
        std::string col;
        std::istringstream row(line);
        while (std::getline(row, col, '\t')) {
            cols.push_back(col);
        }
        if (cols.empty()) {
            continue;
        }
        RemotePresenceRow entry;
        if (cols[0] == "A" && cols.size() >= 3) {
            entry.kind = "active";
            entry.username = cols[1];
            try {
                entry.slot_index = std::stoi(cols[2]);
            } catch (...) {
                continue;
            }
            if (cols.size() >= 4) {
                entry.display_name = cols[3];
            }
            if (cols.size() >= 5) {
                entry.game_id = cols[4];
            }
            out.push_back(std::move(entry));
        } else if (cols[0] == "C" && cols.size() >= 4) {
            entry.kind = "connected";
            entry.username = cols[1];
            try {
                entry.client_id = static_cast<std::uint32_t>(std::stoul(cols[2]));
                entry.slot_index = std::stoi(cols[3]);
            } catch (...) {
                continue;
            }
            if (cols.size() >= 5) {
                entry.phase = cols[4];
            }
            if (cols.size() >= 6) {
                entry.seated = cols[5] == "1";
            }
            out.push_back(std::move(entry));
        }
    }
    return out;
}

/** Write a Connected-client disconnect marker (same as Users-tab Kick). */
inline std::string remote_host_kick_connected_shell(
    std::uint32_t client_id,
    int slot_index,
    const std::string& saves_root = {}) {
    const std::string key = slot_index < 0
        ? ("lobby-" + std::to_string(client_id))
        : ("slot-" + std::to_string(slot_index) + "-" + std::to_string(client_id));
    const std::string json = std::string("{\"reason\":\"kicked\",\"client_id\":")
        + std::to_string(client_id)
        + ",\"slot_index\":"
        + std::to_string(slot_index)
        + "}";
    const std::string root = saves_root.empty()
        ? ("\"" + remote_host_default_saves_root_expr() + "\"")
        : remote_shell_single_quote(saves_root);
    const auto path_expr = std::string("\"$dir/disconnect-") + key + "\"";
    return std::string("set -e; root=")
        + root
        + "; dir=\"$root/.archstreamer_active\"; mkdir -p \"$dir\"; printf '%s\\n' "
        + remote_shell_single_quote(json)
        + " > "
        + path_expr;
}

/** Write an Active-slot stop marker (same as Users-tab Kick of a playing session). */
inline std::string remote_host_kick_active_shell(
    int slot_index,
    const std::string& saves_root = {}) {
    const std::string json = std::string("{\"reason\":\"kicked\",\"slot_index\":")
        + std::to_string(slot_index)
        + "}";
    const std::string root = saves_root.empty()
        ? ("\"" + remote_host_default_saves_root_expr() + "\"")
        : remote_shell_single_quote(saves_root);
    const auto path_expr =
        std::string("\"$dir/stop-slot-") + std::to_string(slot_index) + "\"";
    return std::string("set -e; root=")
        + root
        + "; dir=\"$root/.archstreamer_active\"; mkdir -p \"$dir\"; printf '%s\\n' "
        + remote_shell_single_quote(json)
        + " > "
        + path_expr;
}

} // namespace archstreamer
