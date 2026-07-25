#pragma once

#include "host/retroarch_process.hpp"

#include "common/platform/default_platform.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace archstreamer {

class WindowsRetroArchProcess final : public RetroArchProcess {
public:
    ~WindowsRetroArchProcess() override;

    void launch(const RetroArchLaunchConfig& config) override;
    void stop() override;
    bool running() const override;
    std::optional<int> last_exit_code() const override;
    std::string last_stderr_tail() const override;

private:
    void capture_stderr_tail() const;
    void clear_stderr_log() const;
    void refresh_exit_code() const;

    mutable ChildProcess process_;
    mutable bool launched_ = false;
    mutable std::optional<int> last_exit_code_;
    mutable std::filesystem::path stderr_log_path_;
    mutable std::string last_stderr_tail_;
};

} // namespace archstreamer
