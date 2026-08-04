#include "host/posix_retroarch_process.hpp"

#include "common/platform/paths.hpp"
#include "host/emulator_orphan_reaper.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

/** access(2) does not search PATH; resolve bare names the same way execvp will. */
bool path_is_executable(const std::string& path_or_name) {
    if (path_or_name.find('/') != std::string::npos) {
        return access(path_or_name.c_str(), X_OK) == 0;
    }
    const char* path_env = getenv("PATH");
    if (path_env == nullptr || *path_env == '\0') {
        path_env = "/usr/local/bin:/usr/bin:/bin";
    }
    std::string path{path_env};
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find(':', start);
        const auto dir = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const auto candidate = (dir.empty() ? "." : dir) + "/" + path_or_name;
        if (access(candidate.c_str(), X_OK) == 0) {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return false;
}

} // namespace

namespace archstreamer {
namespace {

constexpr const char* kSessionTokenEnv = "ARCHSTREAMER_SLOT_TOKEN";

std::string path_string(const std::filesystem::path& path, const char* label) {
    if (path.empty()) {
        throw std::runtime_error(std::string(label) + " path is empty");
    }

    return path.string();
}

std::string make_session_token() {
    static std::atomic<std::uint64_t> counter{0};
    std::ostringstream out;
    out << "as-" << getpid() << '-' << counter.fetch_add(1) << '-'
        << std::chrono::steady_clock::now().time_since_epoch().count();
    return out.str();
}

void close_inherited_fds() {
    int max_fd = static_cast<int>(sysconf(_SC_OPEN_MAX));
    if (max_fd < 1024) {
        max_fd = 1024;
    }
    if (max_fd > 65536) {
        max_fd = 65536;
    }
    for (int fd = 3; fd < max_fd; ++fd) {
        close(fd);
    }
}

bool environ_contains_token(const std::filesystem::path& environ_path, const std::string& token) {
    if (token.empty()) {
        return false;
    }
    std::ifstream in(environ_path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string needle = std::string(kSessionTokenEnv) + "=" + token;
    return raw.find(needle) != std::string::npos;
}

std::set<pid_t> collect_pids_for_session(pid_t group, const std::string& token) {
    std::set<pid_t> pids;
    std::error_code ec;
    for (const auto& dir : std::filesystem::directory_iterator("/proc", ec)) {
        if (ec) {
            break;
        }
        const auto name = dir.path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(), ::isdigit)) {
            continue;
        }
        const pid_t pid = static_cast<pid_t>(std::stol(name));
        if (pid <= 1) {
            continue;
        }
        bool match = false;
        if (group > 0) {
            const pid_t pgid = getpgid(pid);
            if (pgid == group) {
                match = true;
            }
        }
        if (!match && !token.empty()
            && environ_contains_token(dir.path() / "environ", token)) {
            match = true;
        }
        if (match) {
            pids.insert(pid);
        }
    }
    return pids;
}

bool any_pid_alive(const std::set<pid_t>& pids) {
    return std::any_of(pids.begin(), pids.end(), [](pid_t pid) {
        return kill(pid, 0) == 0;
    });
}

void signal_pids(const std::set<pid_t>& pids, int sig) {
    for (const pid_t pid : pids) {
        kill(pid, sig);
    }
}

/** flatpak run drops unknown env; pass the session token explicitly. */
void inject_flatpak_session_token(std::vector<std::string>& args, const std::string& token) {
    if (token.empty() || args.empty()) {
        return;
    }
    const std::string env_arg = std::string("--env=") + kSessionTokenEnv + "=" + token;
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] != "run") {
            continue;
        }
        // `flatpak run …` or `flatpak-spawn --host flatpak run …`
        if (i == 0 || (args[i - 1] != "flatpak" && args[i - 1].find("flatpak") == std::string::npos)) {
            continue;
        }
        args.insert(args.begin() + static_cast<std::ptrdiff_t>(i) + 1, env_arg);
        return;
    }
}

std::string read_file_tail(const std::filesystem::path& path, std::size_t max_bytes) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return {};
    }
    const auto size = static_cast<std::size_t>(in.tellg());
    if (size == 0) {
        return {};
    }
    const auto start = size > max_bytes ? size - max_bytes : 0;
    in.seekg(static_cast<std::streamoff>(start));
    std::string data(size - start, '\0');
    in.read(data.data(), static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<std::size_t>(in.gcount()));

    // Drop a partial first line when we seeked mid-file.
    if (start > 0) {
        const auto nl = data.find('\n');
        if (nl != std::string::npos && nl + 1 < data.size()) {
            data.erase(0, nl + 1);
        }
    }

    // Prefer diagnostic lines when present, but keep the full tail as fallback so
    // gamescope/Ryujinx mid-session deaths (abort, Vulkan, etc.) are not dropped.
    std::string filtered;
    std::size_t line_start = 0;
    while (line_start < data.size()) {
        auto line_end = data.find('\n', line_start);
        if (line_end == std::string::npos) {
            line_end = data.size();
        }
        const auto line = data.substr(line_start, line_end - line_start);
        const bool interesting =
            line.find("ERROR") != std::string::npos ||
            line.find("Error") != std::string::npos ||
            line.find("Failed") != std::string::npos ||
            line.find("BIOS") != std::string::npos ||
            line.find("Fatal") != std::string::npos ||
            line.find("fatal") != std::string::npos ||
            line.find("Could not") != std::string::npos ||
            line.find("abort") != std::string::npos ||
            line.find("Abort") != std::string::npos ||
            line.find("Segmentation") != std::string::npos ||
            line.find("segfault") != std::string::npos ||
            line.find("Vulkan") != std::string::npos ||
            line.find("vulkan") != std::string::npos ||
            line.find("gamescope") != std::string::npos ||
            line.find("Ryujinx") != std::string::npos ||
            line.find("Exception") != std::string::npos ||
            line.find("Unhandled") != std::string::npos ||
            line.find("present queue") != std::string::npos ||
            line.find("SIG") != std::string::npos ||
            line.find("killed") != std::string::npos ||
            line.find("Killed") != std::string::npos ||
            line.find("terminated") != std::string::npos;
        if (interesting) {
            if (!filtered.empty()) {
                filtered.push_back('\n');
            }
            filtered += line;
        }
        line_start = line_end + 1;
    }
    if (!filtered.empty()) {
        // Cap filtered output but append a marker that more context exists in the
        // durable log / full tail when we truncated heavily.
        constexpr std::size_t kMaxFiltered = 12288;
        if (filtered.size() > kMaxFiltered) {
            filtered = filtered.substr(filtered.size() - kMaxFiltered);
            const auto nl = filtered.find('\n');
            if (nl != std::string::npos && nl + 1 < filtered.size()) {
                filtered.erase(0, nl + 1);
            }
        }
        return filtered;
    }
    return data;
}

bool looks_like_host_exec_shim(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    for (int i = 0; i < 24 && std::getline(in, line); ++i) {
        if (line.find("distrobox-host-exec") != std::string::npos ||
            line.find("host-spawn") != std::string::npos) {
            return true;
        }
    }
    return false;
}

// distrobox-host-exec / host-spawn often replace the child env with the host session.
// Expand shim → `distrobox-host-exec env -u … KEY=VAL… /usr/bin/retroarch …` so Pulse
// (PULSE_SINK), DISPLAY, and the private XDG_RUNTIME_DIR actually reach RetroArch.
void expand_host_exec_retroarch_shim(
    std::vector<std::string>& args,
    const RetroArchLaunchConfig& config) {
    if (config.standalone || args.empty() || !config.command_prefix.empty()) {
        return;
    }
    if (!looks_like_host_exec_shim(args.front())) {
        return;
    }

    std::vector<std::string> rewritten{
        "/usr/bin/distrobox-host-exec",
        "env",
    };
    auto add_unset = [&](const std::string& key) {
        rewritten.push_back("-u");
        rewritten.push_back(key);
    };
    bool cleared_wayland = false;
    bool cleared_wayland_socket = false;
    for (const auto& key : config.unset_environment) {
        add_unset(key);
        if (key == "WAYLAND_DISPLAY") {
            cleared_wayland = true;
        }
        if (key == "WAYLAND_SOCKET") {
            cleared_wayland_socket = true;
        }
    }
    if (!cleared_wayland) {
        add_unset("WAYLAND_DISPLAY");
    }
    if (!cleared_wayland_socket) {
        add_unset("WAYLAND_SOCKET");
    }
    for (const auto& [key, value] : config.environment) {
        rewritten.push_back(key + "=" + value);
    }
    rewritten.push_back("/usr/bin/retroarch");
    rewritten.insert(rewritten.end(), args.begin() + 1, args.end());
    args = std::move(rewritten);
}

} // namespace

PosixRetroArchProcess::~PosixRetroArchProcess() {
    stop();
    clear_stderr_log();
}

void PosixRetroArchProcess::clear_stderr_log() const {
    if (!stderr_log_path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(stderr_log_path_, ec);
        stderr_log_path_.clear();
    }
}

void PosixRetroArchProcess::capture_stderr_tail() const {
    if (stderr_log_path_.empty()) {
        return;
    }
    last_stderr_tail_ = read_file_tail(stderr_log_path_, 24576);

    // Keep a durable copy for post-mortem after the temp file is removed.
    if (const auto cache = archstreamer_cache_directory(); !cache.empty()) {
        const auto durable = std::filesystem::path(cache) / "last-emu-stdio.log";
        std::error_code ec;
        std::filesystem::create_directories(durable.parent_path(), ec);
        std::filesystem::copy_file(
            stderr_log_path_,
            durable,
            std::filesystem::copy_options::overwrite_existing,
            ec);
        if (!ec) {
            std::cerr << "emulator/gamescope stdio saved to " << durable.string() << '\n';
        }
    }

    clear_stderr_log();
}

void PosixRetroArchProcess::launch(const RetroArchLaunchConfig& config) {
    if (running()) {
        throw std::runtime_error("RetroArch is already running");
    }

    last_exit_code_.reset();
    last_stderr_tail_.clear();
    clear_stderr_log();
    session_token_ = make_session_token();

    // Copy so we can inject the session token without mutating the caller's config.
    RetroArchLaunchConfig launch = config;
    launch.environment.emplace_back(kSessionTokenEnv, session_token_);

    std::vector<std::string> args;
    if (launch.standalone) {
        // Standalone emulator binary lives in core_path (same field catalog uses for display).
        // Optional command_prefix wraps the binary (e.g. vglrun for VirtualGL OpenGL).
        if (!launch.command_prefix.empty()) {
            args = launch.command_prefix;
        }
        args.push_back(path_string(launch.core_path, "standalone emulator"));
        args.insert(
            args.end(),
            launch.standalone_args_before_content.begin(),
            launch.standalone_args_before_content.end());
        args.insert(args.end(), launch.extra_args.begin(), launch.extra_args.end());
        args.push_back(path_string(launch.content_path, "content"));
    } else {
        if (!launch.command_prefix.empty()) {
            args = launch.command_prefix;
        } else {
            args.push_back(path_string(launch.retroarch_path, "RetroArch executable"));
        }
        args.insert(args.end(), launch.extra_args.begin(), launch.extra_args.end());
        args.push_back("-L");
        args.push_back(path_string(launch.core_path, "RetroArch core"));
        args.push_back(path_string(launch.content_path, "RetroArch content"));
    }

    expand_host_exec_retroarch_shim(args, launch);
    inject_flatpak_session_token(args, session_token_);

    if (!launch.network_namespace_prefix.empty()) {
        std::vector<std::string> wrapped = launch.network_namespace_prefix;
        wrapped.insert(wrapped.end(), args.begin(), args.end());
        args = std::move(wrapped);
    }

    const auto binary = launch.standalone
        ? path_string(launch.core_path, "standalone emulator")
        : path_string(launch.retroarch_path, "RetroArch executable");
    if (access(binary.c_str(), X_OK) != 0) {
        throw std::runtime_error(
            std::string(launch.standalone ? "Emulator" : "RetroArch") +
            " executable is missing or not executable: " + binary);
    }
    if (!args.empty() && !path_is_executable(args.front())) {
        throw std::runtime_error(
            "Launch prefix executable is missing or not executable: " + args.front());
    }

    // Quiet mode: redirect stdout+stderr into a temp log so mid-session deaths
    // (gamescope/Ryujinx) leave a diagnosable tail instead of vanishing into /dev/null.
    std::string stderr_path;
    if (launch.quiet_stdio) {
        char tmpl[] = "/tmp/archstreamer-emu-XXXXXX";
        const int fd = mkstemp(tmpl);
        if (fd >= 0) {
            close(fd);
            stderr_path = tmpl;
            stderr_log_path_ = tmpl;
            std::cerr << "emulator/gamescope stdio → " << stderr_path << '\n';
        }
    }

    pid_t child = fork();
    if (child < 0) {
        session_token_.clear();
        clear_stderr_log();
        throw std::runtime_error(std::string("fork failed: ") + std::strerror(errno));
    }

    if (child == 0) {
        // Own process group so stop() can SIGTERM/SIGKILL the whole tree
        // (gamescope + nested Ryujinx/AppImage). Without this, SIGKILL on the
        // wrapper alone orphans grandchildren that then hot-plug onto the next
        // session's virtual pad and PipeWire node.
        if (setsid() < 0) {
            _exit(127);
        }
        close_inherited_fds();
        if (launch.quiet_stdio) {
            const int null_fd = open("/dev/null", O_RDWR);
            int err_fd = -1;
            if (!stderr_path.empty()) {
                err_fd = open(stderr_path.c_str(), O_WRONLY | O_APPEND);
            }
            if (err_fd >= 0) {
                // Capture both streams — gamescope often logs useful progress on stdout.
                dup2(err_fd, STDOUT_FILENO);
                dup2(err_fd, STDERR_FILENO);
                if (err_fd > STDERR_FILENO) {
                    close(err_fd);
                }
            } else if (null_fd >= 0) {
                dup2(null_fd, STDOUT_FILENO);
                dup2(null_fd, STDERR_FILENO);
            }
            if (null_fd > STDERR_FILENO) {
                close(null_fd);
            }
        }
        for (const auto& key : launch.unset_environment) {
            unsetenv(key.c_str());
        }
        for (const auto& [key, value] : launch.environment) {
            setenv(key.c_str(), value.c_str(), 1);
        }

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& arg : args) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);

        execvp(argv[0], argv.data());
        _exit(127);
    }

    pid_ = child;
    // The child calls setsid(), making its pid the process-group id. Keep this
    // separately because the leader may exit before its descendants.
    process_group_id_ = child;
    register_emulator_session_token(session_token_);
}

void PosixRetroArchProcess::stop() {
    const pid_t group = process_group_id_;
    const std::string token = session_token_;
    if (group <= 0 && token.empty()) {
        (void)running();
        return;
    }

    // Snapshot by process group and ARCHSTREAMER_SLOT_TOKEN so AppImage/flatpak
    // re-execs that leave the original setsid group are still terminated.
    auto targets = collect_pids_for_session(group, token);
    if (pid_ > 0) {
        targets.insert(pid_);
    }
    if (group > 0) {
        kill(-group, SIGTERM);
    }
    signal_pids(targets, SIGTERM);

    for (int i = 0; i < 40; ++i) {
        if (pid_ > 0) {
            int status = 0;
            const pid_t result = waitpid(pid_, &status, WNOHANG);
            if (result == pid_) {
                record_status(status);
                pid_ = -1;
            } else if (result < 0 && errno == ECHILD) {
                pid_ = -1;
            }
        }
        targets = collect_pids_for_session(group, token);
        if (pid_ > 0) {
            targets.insert(pid_);
        }
        if (!any_pid_alive(targets)
            && (group <= 0 || (kill(-group, 0) != 0 && errno == ESRCH))) {
            pid_ = -1;
            process_group_id_ = -1;
            unregister_emulator_session_token(session_token_);
            session_token_.clear();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    targets = collect_pids_for_session(group, token);
    if (pid_ > 0) {
        targets.insert(pid_);
    }
    if (group > 0) {
        kill(-group, SIGKILL);
    }
    signal_pids(targets, SIGKILL);
    if (pid_ > 0) {
        int status = 0;
        if (waitpid(pid_, &status, 0) == pid_) {
            record_status(status);
        }
    }
    // Final sweep for stragglers that reparented during the kill window.
    targets = collect_pids_for_session(group, token);
    signal_pids(targets, SIGKILL);
    if (any_pid_alive(targets)) {
        std::cerr
            << "Warning: emulator session tree still alive after SIGKILL"
            << (token.empty() ? "" : (" token=" + token))
            << "; will retry on next stop/destructor\n";
        pid_ = -1;
        // Keep process_group_id_ / session_token_ so a later stop() can find them.
        // Unregister so mid-lobby stale reap can also clean them up.
        unregister_emulator_session_token(token);
        return;
    }
    pid_ = -1;
    process_group_id_ = -1;
    unregister_emulator_session_token(session_token_);
    session_token_.clear();
}

bool PosixRetroArchProcess::running() const {
    if (pid_ > 0) {
        int status = 0;
        const pid_t result = waitpid(pid_, &status, WNOHANG);
        if (result == 0) {
            return true;
        }

        if (result == pid_) {
            record_status(status);
            pid_ = -1;
        } else if (errno == ECHILD) {
            pid_ = -1;
            if (!last_exit_code_.has_value()) {
                last_exit_code_ = -1;
            }
            capture_stderr_tail();
        } else if (result < 0) {
            return true;
        }
    }

    // Leader may have exited while gamescope/AppImage grandchildren remain.
    if (process_group_id_ > 0 && kill(-process_group_id_, 0) == 0) {
        return true;
    }
    if (!session_token_.empty()) {
        const auto leftovers = collect_pids_for_session(process_group_id_, session_token_);
        if (any_pid_alive(leftovers)) {
            return true;
        }
    }

    if (process_group_id_ > 0 || !session_token_.empty()) {
        process_group_id_ = -1;
        unregister_emulator_session_token(session_token_);
        session_token_.clear();
        if (pid_ <= 0 && !last_exit_code_.has_value()) {
            last_exit_code_ = -1;
        }
        capture_stderr_tail();
    }
    return false;
}

std::optional<int> PosixRetroArchProcess::last_exit_code() const {
    return last_exit_code_;
}

std::string PosixRetroArchProcess::last_stderr_tail() const {
    return last_stderr_tail_;
}

std::optional<int> PosixRetroArchProcess::process_id() const {
    if (pid_ <= 0) {
        return std::nullopt;
    }
    return static_cast<int>(pid_);
}

void PosixRetroArchProcess::record_status(int status) const {
    if (WIFEXITED(status)) {
        last_exit_code_ = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        last_exit_code_ = 128 + WTERMSIG(status);
    } else {
        last_exit_code_ = -1;
    }
    capture_stderr_tail();
}

} // namespace archstreamer
