#pragma once

#include "host/retroarch_process.hpp"

#include <filesystem>
#include <string>
#include <sys/types.h>

namespace archstreamer {

class PosixRetroArchProcess final : public RetroArchProcess {
public:
    ~PosixRetroArchProcess() override;

    void launch(const RetroArchLaunchConfig& config) override;
    void stop() override;
    bool running() const override;
    std::optional<int> last_exit_code() const override;
    std::string last_stderr_tail() const override;
    std::optional<int> process_id() const override;

private:
    void record_status(int status) const;
    void capture_stderr_tail() const;
    void clear_stderr_log() const;

    mutable pid_t pid_ = -1;
    // Retained after the leader exits so stop() can still kill surviving
    // gamescope/Ryujinx grandchildren in the session created by setsid().
    mutable pid_t process_group_id_ = -1;
    mutable std::optional<int> last_exit_code_;
    mutable std::filesystem::path stderr_log_path_;
    mutable std::string last_stderr_tail_;
};

} // namespace archstreamer
