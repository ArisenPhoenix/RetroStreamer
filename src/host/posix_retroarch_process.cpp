#include "host/posix_retroarch_process.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
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

namespace archstreamer {
namespace {

std::string path_string(const std::filesystem::path& path, const char* label) {
    if (path.empty()) {
        throw std::runtime_error(std::string(label) + " path is empty");
    }

    return path.string();
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

    // Prefer the most useful diagnostic lines for GUI errors.
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
            line.find("Failed") != std::string::npos ||
            line.find("BIOS") != std::string::npos ||
            line.find("Fatal") != std::string::npos ||
            line.find("Could not") != std::string::npos;
        if (interesting) {
            if (!filtered.empty()) {
                filtered.push_back('\n');
            }
            filtered += line;
        }
        line_start = line_end + 1;
    }
    if (!filtered.empty()) {
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
    last_stderr_tail_ = read_file_tail(stderr_log_path_, 4096);
    clear_stderr_log();
}

void PosixRetroArchProcess::launch(const RetroArchLaunchConfig& config) {
    if (running()) {
        throw std::runtime_error("RetroArch is already running");
    }

    last_exit_code_.reset();
    last_stderr_tail_.clear();
    clear_stderr_log();

    std::vector<std::string> args;
    if (config.standalone) {
        // Standalone emulator binary lives in core_path (same field catalog uses for display).
        // Optional command_prefix wraps the binary (e.g. vglrun for VirtualGL OpenGL).
        if (!config.command_prefix.empty()) {
            args = config.command_prefix;
        }
        args.push_back(path_string(config.core_path, "standalone emulator"));
        args.insert(
            args.end(),
            config.standalone_args_before_content.begin(),
            config.standalone_args_before_content.end());
        args.insert(args.end(), config.extra_args.begin(), config.extra_args.end());
        args.push_back(path_string(config.content_path, "content"));
    } else {
        if (!config.command_prefix.empty()) {
            args = config.command_prefix;
        } else {
            args.push_back(path_string(config.retroarch_path, "RetroArch executable"));
        }
        args.insert(args.end(), config.extra_args.begin(), config.extra_args.end());
        args.push_back("-L");
        args.push_back(path_string(config.core_path, "RetroArch core"));
        args.push_back(path_string(config.content_path, "RetroArch content"));
    }

    expand_host_exec_retroarch_shim(args, config);

    const auto& executable = config.standalone
        ? path_string(config.core_path, "standalone emulator")
        : args.front();
    if (access(executable.c_str(), X_OK) != 0) {
        throw std::runtime_error(
            std::string(config.standalone ? "Emulator" : "RetroArch") +
            " executable is missing or not executable: " + executable);
    }
    if (!config.command_prefix.empty() && access(args.front().c_str(), X_OK) != 0) {
        throw std::runtime_error(
            "Launch prefix executable is missing or not executable: " + args.front());
    }

    // Quiet mode: keep stdout silent, but capture stderr so early exits (missing BIOS, etc.)
    // can be surfaced in the host error dialog.
    std::string stderr_path;
    if (config.quiet_stdio) {
        char tmpl[] = "/tmp/archstreamer-emu-XXXXXX";
        const int fd = mkstemp(tmpl);
        if (fd >= 0) {
            close(fd);
            stderr_path = tmpl;
            stderr_log_path_ = tmpl;
        }
    }

    pid_t child = fork();
    if (child < 0) {
        clear_stderr_log();
        throw std::runtime_error(std::string("fork failed: ") + std::strerror(errno));
    }

    if (child == 0) {
        close_inherited_fds();
        if (config.quiet_stdio) {
            const int null_fd = open("/dev/null", O_RDWR);
            if (null_fd >= 0) {
                dup2(null_fd, STDOUT_FILENO);
            }
            int err_fd = -1;
            if (!stderr_path.empty()) {
                err_fd = open(stderr_path.c_str(), O_WRONLY | O_APPEND);
            }
            if (err_fd >= 0) {
                dup2(err_fd, STDERR_FILENO);
                if (err_fd > STDERR_FILENO) {
                    close(err_fd);
                }
            } else if (null_fd >= 0) {
                dup2(null_fd, STDERR_FILENO);
            }
            if (null_fd > STDERR_FILENO) {
                close(null_fd);
            }
        }
        for (const auto& key : config.unset_environment) {
            unsetenv(key.c_str());
        }
        for (const auto& [key, value] : config.environment) {
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
}

void PosixRetroArchProcess::stop() {
    if (!running()) {
        pid_ = -1;
        return;
    }

    kill(pid_, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        int status = 0;
        const pid_t result = waitpid(pid_, &status, WNOHANG);
        if (result == pid_) {
            record_status(status);
            pid_ = -1;
            return;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    kill(pid_, SIGKILL);
    int status = 0;
    waitpid(pid_, &status, 0);
    record_status(status);
    pid_ = -1;
}

bool PosixRetroArchProcess::running() const {
    if (pid_ <= 0) {
        return false;
    }

    int status = 0;
    const pid_t result = waitpid(pid_, &status, WNOHANG);
    if (result == 0) {
        return true;
    }

    if (result == pid_) {
        record_status(status);
        pid_ = -1;
        return false;
    }

    if (errno == ECHILD) {
        pid_ = -1;
        return false;
    }

    return false;
}

std::optional<int> PosixRetroArchProcess::last_exit_code() const {
    return last_exit_code_;
}

std::string PosixRetroArchProcess::last_stderr_tail() const {
    return last_stderr_tail_;
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
