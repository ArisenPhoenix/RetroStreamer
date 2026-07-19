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

private:
    mutable pid_t pid_ = -1;
};

} // namespace archstreamer
