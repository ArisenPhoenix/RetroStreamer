#pragma once

#ifdef _WIN32

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace archstreamer {

class WindowsChildProcess {
public:
    WindowsChildProcess() = default;
    ~WindowsChildProcess();

    WindowsChildProcess(const WindowsChildProcess&) = delete;
    WindowsChildProcess& operator=(const WindowsChildProcess&) = delete;

    WindowsChildProcess(WindowsChildProcess&& other) noexcept;
    WindowsChildProcess& operator=(WindowsChildProcess&& other) noexcept;

    void start(
        const std::vector<std::string>& args,
        const std::vector<std::pair<std::string, std::string>>& env = {},
        const std::vector<std::string>& unset_env = {},
        const std::string& stderr_path = {});
    void stop();
    bool running() const;

private:
    void close_handles();

    PROCESS_INFORMATION process_info_{};
    bool started_ = false;
};

} // namespace archstreamer

#endif // _WIN32
