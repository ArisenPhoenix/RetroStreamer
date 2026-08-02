#include "host/switch/yuzu_user_profile.hpp"

#include "host/switch/default_switch_paths.hpp"
#include "host/switch/qt_ini_editor.hpp"
#include "host/switch/switch_fs.hpp"
#include "host/switch/switch_system_defaults.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace archstreamer {

void YuzuUserProfileService::ensure_qt_config(
    const YuzuUserProfile& profile,
    bool force_opengl,
    bool force_vulkan,
    int vulkan_device,
    int resolution_scale) {
    const auto config_dir = profile.xdg_config_home / "yuzu";
    const auto config_path = config_dir / "qt-config.ini";
    std::filesystem::create_directories(config_dir);

    const auto yuzu_data = profile.xdg_data_home / "yuzu";
    const auto nand = (yuzu_data / "nand").string();
    const auto sdmc = (yuzu_data / "sdmc").string();
    const auto load = (yuzu_data / "load").string();
    const auto dump = (yuzu_data / "dump").string();
    const auto tas = (yuzu_data / "tas").string();
    std::filesystem::create_directories(yuzu_data / "tas");
    std::filesystem::create_directories(yuzu_data / "screenshots");
    std::filesystem::create_directories(yuzu_data / "nand" / "system" / "Contents" / "registered");
    std::filesystem::create_directories(yuzu_data / "nand" / "user" / "Contents" / "registered");
    std::filesystem::create_directories(yuzu_data / "sdmc" / "Nintendo" / "Contents" / "registered");

    std::string contents;
    if (std::filesystem::exists(config_path)) {
        std::ifstream in(config_path);
        contents.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    } else {
        contents =
            "[Data%20Storage]\n"
            "use_virtual_sd=true\n"
            "use_virtual_sd\\default=true\n"
            "nand_directory=" +
            nand +
            "\n"
            "nand_directory\\default=false\n"
            "sdmc_directory=" +
            sdmc +
            "\n"
            "sdmc_directory\\default=false\n"
            "load_directory=" +
            load +
            "\n"
            "load_directory\\default=false\n"
            "dump_directory=" +
            dump +
            "\n"
            "dump_directory\\default=false\n"
            "tas_directory=" +
            tas +
            "\n"
            "tas_directory\\default=false\n"
            "\n"
            "[UI]\n"
            "confirmStop=0\n"
            "confirmStop\\default=false\n"
            "firstStart=false\n"
            "firstStart\\default=false\n"
            "calloutFlags=1\n"
            "calloutFlags\\default=false\n"
            "\n"
            "[Renderer]\n"
            "backend=0\n"
            "backend\\default=false\n"
            "\n"
            "[WebService]\n"
            "enable_telemetry=false\n"
            "enable_telemetry\\default=false\n"
            "web_api_url=https://api.yuzu-emu.org\n"
            "web_api_url\\default=true\n"
            "yuzu_token=\n"
            "yuzu_token\\default=true\n"
            "yuzu_username=\n"
            "yuzu_username\\default=true\n";
    }

    set_qt_ini_value(contents, "nand_directory", nand);
    set_qt_ini_value(contents, "nand_directory\\default", "false");
    set_qt_ini_value(contents, "sdmc_directory", sdmc);
    set_qt_ini_value(contents, "sdmc_directory\\default", "false");
    set_qt_ini_value(contents, "load_directory", load);
    set_qt_ini_value(contents, "load_directory\\default", "false");
    set_qt_ini_value(contents, "dump_directory", dump);
    set_qt_ini_value(contents, "dump_directory\\default", "false");
    set_qt_ini_value(contents, "tas_directory", tas);
    set_qt_ini_value(contents, "tas_directory\\default", "false");
    set_qt_ini_value(contents, "firstStart", "false");
    set_qt_ini_value(contents, "firstStart\\default", "false");
    set_qt_ini_value(contents, "calloutFlags", "1");
    set_qt_ini_value(contents, "calloutFlags\\default", "false");
    set_qt_ini_value(contents, "enable_telemetry", "false");
    set_qt_ini_value(contents, "enable_telemetry\\default", "false");
    set_qt_ini_value(contents, "confirmStop", "0");
    if (force_opengl) {
        set_qt_ini_group_value(contents, "Renderer", "backend", "0");
        set_qt_ini_group_value(contents, "Renderer", "backend\\default", "false");
        set_qt_ini_group_value(contents, "Renderer", "shader_backend", "0");
        set_qt_ini_group_value(contents, "Renderer", "shader_backend\\default", "false");
        set_qt_ini_group_value(contents, "Renderer", "perform_vulkan_check", "false");
        set_qt_ini_group_value(contents, "Renderer", "perform_vulkan_check\\default", "false");
        set_qt_ini_group_value(contents, "UI", "fullscreen", "false");
        set_qt_ini_group_value(contents, "UI", "fullscreen\\default", "false");
    } else if (force_vulkan) {
        set_qt_ini_group_value(contents, "Renderer", "backend", "1");
        set_qt_ini_group_value(contents, "Renderer", "backend\\default", "false");
        set_qt_ini_group_value(contents, "Renderer", "perform_vulkan_check", "false");
        set_qt_ini_group_value(contents, "Renderer", "perform_vulkan_check\\default", "false");
    }
    if (vulkan_device >= 0) {
        set_qt_ini_group_value(contents, "Renderer", "vulkan_device", std::to_string(vulkan_device));
        set_qt_ini_group_value(contents, "Renderer", "vulkan_device\\default", "false");
    }
    if (resolution_scale > 0) {
        const int scale = std::clamp(resolution_scale, 1, 6);
        const int setup = scale + 1;
        set_qt_ini_group_value(contents, "Renderer", "resolution_setup", std::to_string(setup));
        set_qt_ini_group_value(contents, "Renderer", "resolution_setup\\default", "false");
    }

    // Continuous fast-forward: this Yuzu build exposes "Toggle Framerate Limit" (no
    // hold-turbo / Toggle Speed Limit hotkey). Remoted F8 toggles it on/off.
    // Free F8 from Change Adapting Filter so the shortcut is unambiguous.
    set_qt_ini_group_value(
        contents, "UI", "Shortcuts\\Main%20Window\\Toggle%20Framerate%20Limit\\KeySeq", "F8");
    set_qt_ini_group_value(
        contents,
        "UI",
        "Shortcuts\\Main%20Window\\Toggle%20Framerate%20Limit\\KeySeq\\default",
        "false");
    set_qt_ini_group_value(
        contents, "UI", "Shortcuts\\Main%20Window\\Change%20Adapting%20Filter\\KeySeq", "");
    set_qt_ini_group_value(
        contents,
        "UI",
        "Shortcuts\\Main%20Window\\Change%20Adapting%20Filter\\KeySeq\\default",
        "false");

    std::ofstream out(config_path, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to write Yuzu qt-config.ini: " + config_path.string());
    }
    out << contents;
}

YuzuUserProfile YuzuUserProfileService::prepare(
    const SaveProfile& save_profile,
    bool force_opengl,
    bool force_vulkan,
    int vulkan_device,
    int resolution_scale) {
    YuzuUserProfile profile;
    profile.xdg_data_home = save_profile.user_directory / "yuzu" / "xdg-data";
    profile.xdg_config_home = save_profile.user_directory / "yuzu" / "xdg-config";
    profile.keys_directory = profile.xdg_data_home / "yuzu" / "keys";

    std::filesystem::create_directories(profile.xdg_data_home / "yuzu");
    std::filesystem::create_directories(profile.xdg_config_home / "yuzu");
    std::filesystem::create_directories(profile.keys_directory);
    std::filesystem::create_directories(profile.xdg_data_home / "yuzu" / "nand");
    std::filesystem::create_directories(profile.xdg_data_home / "yuzu" / "sdmc");
    std::filesystem::create_directories(profile.xdg_data_home / "yuzu" / "load");
    std::filesystem::create_directories(profile.xdg_data_home / "yuzu" / "screenshot");
    std::filesystem::create_directories(profile.xdg_data_home / "yuzu" / "dump");

    const auto shared_keys = SwitchSystemDefaults::keys_directory();
    SwitchSystemDefaults::ensure();
    switch_copy_key_files(shared_keys, profile.keys_directory);

    if (!std::filesystem::exists(profile.keys_directory / "prod.keys")) {
        switch_copy_key_files(SwitchPaths::yuzu_runtime_root() / "keys", profile.keys_directory);
    }
    if (!std::filesystem::exists(profile.keys_directory / "prod.keys")) {
        if (const auto source_keys = SwitchSystemDefaults::find_source_keys_dir(); source_keys.has_value()) {
            switch_copy_key_files(*source_keys, profile.keys_directory);
        }
    }

    ensure_qt_config(profile, force_opengl, force_vulkan, vulkan_device, resolution_scale);
    return profile;
}

} // namespace archstreamer
