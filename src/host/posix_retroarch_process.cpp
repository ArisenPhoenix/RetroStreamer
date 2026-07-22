#include "host/posix_retroarch_process.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

} // namespace

PosixRetroArchProcess::~PosixRetroArchProcess() {
    stop();
}

void PosixRetroArchProcess::launch(const RetroArchLaunchConfig& config) {
    if (running()) {
        throw std::runtime_error("RetroArch is already running");
    }

    last_exit_code_.reset();

    std::vector<std::string> args;
    if (!config.command_prefix.empty()) {
        args = config.command_prefix;
    } else {
        args.push_back(path_string(config.retroarch_path, "RetroArch executable"));
    }
    args.insert(args.end(), config.extra_args.begin(), config.extra_args.end());
    args.push_back("-L");
    args.push_back(path_string(config.core_path, "RetroArch core"));
    args.push_back(path_string(config.content_path, "RetroArch content"));

    if (config.command_prefix.empty() && access(args.front().c_str(), X_OK) != 0) {
        throw std::runtime_error(
            "RetroArch executable is missing or not executable: " + args.front());
    }

    pid_t child = fork();
    if (child < 0) {
        throw std::runtime_error(std::string("fork failed: ") + std::strerror(errno));
    }

    if (child == 0) {
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

void PosixRetroArchProcess::record_status(int status) const {
    if (WIFEXITED(status)) {
        last_exit_code_ = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        last_exit_code_ = 128 + WTERMSIG(status);
    } else {
        last_exit_code_ = -1;
    }
}

} // namespace archstreamer
