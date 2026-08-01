#include "host/switch/ryujinx_launch_env.hpp"

namespace archstreamer {

std::vector<std::pair<std::string, std::string>> RyujinxLaunchEnv::launch_environment(
    const RyujinxUserProfile& profile) {
    std::vector<std::pair<std::string, std::string>> env{
        {"XDG_CONFIG_HOME", profile.xdg_config_home.string()},
        {"SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1"},
    };
    if (!profile.sdl_gamecontroller_config.empty()) {
        env.emplace_back("SDL_GAMECONTROLLERCONFIG", profile.sdl_gamecontroller_config);
    }
    return env;
}

} // namespace archstreamer
