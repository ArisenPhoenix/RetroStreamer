#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace archstreamer {

struct RetroArchLaunchConfig {
    std::filesystem::path retroarch_path;
    std::filesystem::path core_path;
    std::filesystem::path content_path;
    std::vector<std::string> extra_args;
    std::vector<std::pair<std::string, std::string>> environment;
};

class RetroArchProcess {
public:
    virtual ~RetroArchProcess() = default;

    virtual void launch(const RetroArchLaunchConfig& config) = 0;
    virtual void stop() = 0;
    virtual bool running() const = 0;
};

} // namespace archstreamer
