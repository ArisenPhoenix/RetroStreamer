#pragma once

#include <filesystem>
#include <optional>
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
    // Optional argv prefix before RetroArch flags (e.g. flatpak run org.libretro.RetroArch).
    std::vector<std::string> command_prefix;
};

class RetroArchProcess {
public:
    virtual ~RetroArchProcess() = default;

    virtual void launch(const RetroArchLaunchConfig& config) = 0;
    virtual void stop() = 0;
    virtual bool running() const = 0;
    virtual std::optional<int> last_exit_code() const { return std::nullopt; }
};

} // namespace archstreamer
