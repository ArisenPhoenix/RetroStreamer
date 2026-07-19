#pragma once

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

namespace archstreamer {

class PosixChildProcess {
public:
    PosixChildProcess() = default;

    ~PosixChildProcess() {
        stop();
    }

    PosixChildProcess(const PosixChildProcess&) = delete;
    PosixChildProcess& operator=(const PosixChildProcess&) = delete;

    PosixChildProcess(PosixChildProcess&& other) noexcept : pid_(std::exchange(other.pid_, -1)) {
    }

    PosixChildProcess& operator=(PosixChildProcess&& other) noexcept {
        if (this != &other) {
            stop();
            pid_ = std::exchange(other.pid_, -1);
        }
        return *this;
    }

    void start(
        std::vector<std::string> args,
        const std::vector<std::pair<std::string, std::string>>& environment = {}) {
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
            for (const auto& [key, value] : environment) {
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

    void stop() {
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

    bool running() const {
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

private:
    mutable pid_t pid_ = -1;
};

} // namespace archstreamer
