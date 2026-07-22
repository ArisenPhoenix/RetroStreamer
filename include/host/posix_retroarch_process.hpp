#pragma once

#include "host/retroarch_process.hpp"

#include <sys/types.h>

namespace archstreamer {

class PosixRetroArchProcess final : public RetroArchProcess {
public:
    ~PosixRetroArchProcess() override;

    void launch(const RetroArchLaunchConfig& config) override;
    void stop() override;
    bool running() const override;
    std::optional<int> last_exit_code() const override;

private:
    void record_status(int status) const;

    mutable pid_t pid_ = -1;
    mutable std::optional<int> last_exit_code_;
};

} // namespace archstreamer
