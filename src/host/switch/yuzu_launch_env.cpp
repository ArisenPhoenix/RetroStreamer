#include "host/switch/yuzu_launch_env.hpp"

namespace archstreamer {

std::vector<std::pair<std::string, std::string>> YuzuLaunchEnv::launch_environment(
    const YuzuUserProfile& profile) {
    return {
        {"XDG_DATA_HOME", profile.xdg_data_home.string()},
        {"XDG_CONFIG_HOME", profile.xdg_config_home.string()},
        {"SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1"},
    };
}

} // namespace archstreamer
