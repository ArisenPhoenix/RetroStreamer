#include "common/platform/posix_process.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <thread>

#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace archstreamer {
namespace {

void close_inherited_fds() {
    // Keep stdin/stdout/stderr; drop everything else so host sockets (especially
    // the controller UDP port) are not shared with Xvfb/GStreamer/RetroArch.
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

} // namespace

PosixChildProcess::~PosixChildProcess() {
    stop();
}

PosixChildProcess::PosixChildProcess(PosixChildProcess&& other) noexcept : pid_(std::exchange(other.pid_, -1)) {
}

PosixChildProcess& PosixChildProcess::operator=(PosixChildProcess&& other) noexcept {
    if (this != &other) {
        stop();
        pid_ = std::exchange(other.pid_, -1);
    }
    return *this;
}

void PosixChildProcess::start(
    std::vector<std::string> args,
    const std::vector<std::pair<std::string, std::string>>& environment,
    const std::vector<std::string>& unset_environment,
    const std::optional<std::string>& stderr_path) {
    if (args.empty()) {
        throw std::runtime_error("cannot start empty command");
    }
    if (running()) {
        throw std::runtime_error("child process is already running");
    }

    const pid_t child = fork();
    if (child < 0) {
        throw std::runtime_error(std::string("fork failed: ") + std::strerror(errno));
    }

    if (child == 0) {
        close_inherited_fds();
        for (const auto& key : unset_environment) {
            unsetenv(key.c_str());
        }
        for (const auto& [key, value] : environment) {
            setenv(key.c_str(), value.c_str(), 1);
        }
        if (stderr_path.has_value()) {
            // Capture both stdout and stderr: gst-launch prints pipeline status /
            // progressreport on stdout, while plugin warnings go to stderr.
            const int fd = open(
                stderr_path->c_str(),
                O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                0644);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                if (fd != STDOUT_FILENO && fd != STDERR_FILENO) {
                    close(fd);
                }
            }
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

void PosixChildProcess::stop() {
    if (!running()) {
        pid_ = -1;
        return;
    }

    kill(pid_, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        int status = 0;
        const pid_t result = waitpid(pid_, &status, WNOHANG);
        if (result == pid_) {
            pid_ = -1;
            return;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    kill(pid_, SIGKILL);
    waitpid(pid_, nullptr, 0);
    pid_ = -1;
}

bool PosixChildProcess::running() const {
    if (pid_ <= 0) {
        return false;
    }

    int status = 0;
    const pid_t result = waitpid(pid_, &status, WNOHANG);
    if (result == 0) {
        return true;
    }

    if (result == pid_ || errno == ECHILD) {
        pid_ = -1;
        return false;
    }

    return true;
}

} // namespace archstreamer
