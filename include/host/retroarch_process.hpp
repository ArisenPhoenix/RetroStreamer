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
    // Optional argv prefix (flatpak run …, or vglrun for VirtualGL standalone launches).
    std::vector<std::string> command_prefix;
    // Standalone emulator (Yuzu, etc.): run executable + args + content, no -L core.
    bool standalone = false;
    std::vector<std::string> standalone_args_before_content;
    // Drop child stdout/stderr (gamescope/Yuzu chatter) unless --verbose.
    bool quiet_stdio = false;
    // Cleared in the child before applying environment (e.g. WAYLAND_DISPLAY).
    std::vector<std::string> unset_environment;
};

class RetroArchProcess {
public:
    virtual ~RetroArchProcess() = default;

    virtual void launch(const RetroArchLaunchConfig& config) = 0;
    virtual void stop() = 0;
    virtual bool running() const = 0;
    virtual std::optional<int> last_exit_code() const { return std::nullopt; }
    // Tail of child stderr when quiet_stdio captured it (empty if unavailable).
    virtual std::string last_stderr_tail() const { return {}; }
    // OS process id of the launched child (gamescope wrapper when used), if known.
    virtual std::optional<int> process_id() const { return std::nullopt; }
};

} // namespace archstreamer
