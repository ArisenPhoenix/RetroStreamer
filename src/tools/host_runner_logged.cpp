#include "common/remote_host.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

bool has_arg(int argc, char** argv, const char* needle) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], needle) == 0) {
            return true;
        }
    }
    return false;
}

std::uint16_t parse_control_port(int argc, char** argv) {
    constexpr auto kDefault = archstreamer::RemoteDefaultControlPort;
    const std::string flag = "--control-port";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] != nullptr ? argv[i] : "";
        std::string value;
        if (arg == flag && i + 1 < argc) {
            value = argv[i + 1] != nullptr ? argv[i + 1] : "";
        } else if (arg.rfind(flag + "=", 0) == 0) {
            value = arg.substr(flag.size() + 1);
        } else {
            continue;
        }
        try {
            const auto parsed = std::stoul(value);
            if (parsed > 0 && parsed <= 65535) {
                return static_cast<std::uint16_t>(parsed);
            }
        } catch (...) {
            return kDefault;
        }
    }
    return kDefault;
}

std::filesystem::path log_directory() {
    if (const char* configured = std::getenv("ARCHSTREAMER_HOST_LOG_DIR");
        configured != nullptr && configured[0] != '\0') {
        return configured;
    }
    return std::filesystem::temp_directory_path() / "archstreamer-logs";
}

std::filesystem::path sibling_host_runner(const char* argv0) {
    std::error_code ec;
    auto self = std::filesystem::weakly_canonical(argv0 != nullptr ? argv0 : "", ec);
    if (ec || self.empty()) {
        self = argv0 != nullptr ? std::filesystem::path(argv0) : std::filesystem::path();
    }
    auto dir = self.parent_path();
    if (dir.empty()) {
        dir = std::filesystem::current_path(ec);
        if (ec) {
            dir = ".";
        }
    }
    return dir / "host_runner";
}

bool redirect_to_log(const std::filesystem::path& log_path) {
    const int fd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        std::cerr << "host_runner_logged: failed to open log \""
                  << log_path.string() << "\": " << std::strerror(errno) << '\n';
        return false;
    }
    if (::dup2(fd, STDOUT_FILENO) < 0 || ::dup2(fd, STDERR_FILENO) < 0) {
        std::cerr << "host_runner_logged: failed to redirect output to \""
                  << log_path.string() << "\": " << std::strerror(errno) << '\n';
        ::close(fd);
        return false;
    }
    if (fd > STDERR_FILENO) {
        ::close(fd);
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const auto runner = sibling_host_runner(argc > 0 ? argv[0] : nullptr);

    // Keep discovery commands usable when this wrapper is supplied as the host binary.
    if (!has_arg(argc, argv, "--list-gpus")) {
        const auto control_port = parse_control_port(argc, argv);
        const auto dir = log_directory();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            std::cerr << "host_runner_logged: failed to create log directory \""
                      << dir.string() << "\": " << ec.message() << '\n';
            return 1;
        }

        const auto log_path = dir / ("host_" + std::to_string(control_port) + ".log");
        {
            std::ofstream header(log_path, std::ios::trunc);
            if (!header) {
                std::cerr << "host_runner_logged: failed to create log \""
                          << log_path.string() << "\"\n";
                return 1;
            }
            header << "host_runner_logged: exec " << runner.string() << '\n'
                   << "host_runner_logged: log " << log_path.string() << '\n';
        }
        if (!redirect_to_log(log_path)) {
            return 1;
        }
    }

    std::vector<char*> child_argv;
    child_argv.reserve(static_cast<std::size_t>(argc) + 1);
    auto runner_string = runner.string();
    child_argv.push_back(runner_string.data());
    for (int i = 1; i < argc; ++i) {
        child_argv.push_back(argv[i]);
    }
    child_argv.push_back(nullptr);

    ::execv(runner_string.c_str(), child_argv.data());
    std::cerr << "host_runner_logged: failed to exec \"" << runner_string
              << "\": " << std::strerror(errno) << '\n';
    return 127;
}
