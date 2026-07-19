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

    std::vector<std::string> args;
    args.push_back(path_string(config.retroarch_path, "RetroArch executable"));
    args.insert(args.end(), config.extra_args.begin(), config.extra_args.end());
    args.push_back("-L");
    args.push_back(path_string(config.core_path, "RetroArch core"));
    args.push_back(path_string(config.content_path, "RetroArch content"));

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

        execv(argv[0], argv.data());
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
            pid_ = -1;
            return;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    kill(pid_, SIGKILL);
    waitpid(pid_, nullptr, 0);
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
        pid_ = -1;
        return false;
    }

    if (errno == ECHILD) {
        pid_ = -1;
        return false;
    }

    return false;
}

} // namespace archstreamer
