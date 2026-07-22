#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <sys/types.h>

namespace archstreamer {

class PosixChildProcess {
public:
    PosixChildProcess() = default;
    ~PosixChildProcess();

    PosixChildProcess(const PosixChildProcess&) = delete;
    PosixChildProcess& operator=(const PosixChildProcess&) = delete;

    PosixChildProcess(PosixChildProcess&& other) noexcept;
    PosixChildProcess& operator=(PosixChildProcess&& other) noexcept;

    void start(
        std::vector<std::string> args,
        const std::vector<std::pair<std::string, std::string>>& environment = {},
        const std::vector<std::string>& unset_environment = {},
        const std::optional<std::string>& stderr_path = std::nullopt);
    void stop();
    bool running() const;

private:
    mutable pid_t pid_ = -1;
};

} // namespace archstreamer
