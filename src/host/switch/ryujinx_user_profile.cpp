#include "host/switch/ryujinx_user_profile.hpp"

#include "host/switch/default_switch_paths.hpp"
#include "host/switch/ryujinx_title_updates.hpp"
#include "host/switch/switch_fs.hpp"
#include "host/switch/switch_system_defaults.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace archstreamer {
namespace {

void ensure_ryujinx_config(
    const std::filesystem::path& config_path,
    bool enable_ldn_mitm,
    int resolution_scale,
    const std::filesystem::path& title_updates_dir,
    const std::string& lan_interface_id) {
    std::filesystem::create_directories(config_path.parent_path());
    nlohmann::json cfg = nlohmann::json::object();
    if (std::filesystem::is_regular_file(config_path)) {
        try {
            std::ifstream in(config_path);
            cfg = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/true);
            if (!cfg.is_object()) {
                cfg = nlohmann::json::object();
            }
        } catch (const nlohmann::json::exception&) {
            cfg = nlohmann::json::object();
        }
    }

    cfg["multiplayer_mode"] = enable_ldn_mitm ? 2 : 1;
    cfg["enable_internet_access"] = false;
    cfg["enable_vsync"] = true;
    // Switch = normal 60Hz cap. Custom + 200% is ArchStreamer FF (~2x). F1 cycles
    // Switch → Unbounded → Custom → Switch; we tap twice to land on Custom.
    cfg["vsync_mode"] = 0;
    cfg["enable_custom_vsync_interval"] = true;
    cfg["custom_vsync_interval"] = 200;
    cfg["dram_size"] = 3;
    cfg["check_updates_on_start"] = false;
    cfg["update_checker_type"] = "Disabled";
    cfg["show_confirm_exit"] = false;
    cfg["skip_user_profiles"] = true;
    cfg["ignore_applet"] = true;
    cfg["enable_discord_integration"] = false;
    cfg["disable_input_when_out_of_focus"] = false;

    // Turbo @ 200% via F6 hold (absolute on/off — VSync F1 cycling desyncs under gamescope).
    cfg["tick_scalar"] = 200;

    if (!title_updates_dir.empty()) {
        cfg["autoload_dirs"] = nlohmann::json::array({title_updates_dir.string()});
    }
    if (!lan_interface_id.empty()) {
        cfg["multiplayer_lan_interface_id"] = lan_interface_id;
    }

    if (!cfg.contains("hotkeys") || !cfg["hotkeys"].is_object()) {
        cfg["hotkeys"] = nlohmann::json::object();
    }
    cfg["hotkeys"]["turbo_mode"] = "F6";
    cfg["hotkeys"]["turbo_mode_while_held"] = true;
    cfg["hotkeys"]["toggle_vsync_mode"] = "F1";
    if (!cfg["hotkeys"].contains("pause") || cfg["hotkeys"]["pause"] == "Unbound" ||
        cfg["hotkeys"]["pause"] == nullptr) {
        cfg["hotkeys"]["pause"] = "F5";
    }

    if (resolution_scale > 0) {
        cfg["res_scale"] = std::clamp(resolution_scale, 1, 4);
    }

    std::ofstream out(config_path, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to write Ryujinx Config.json: " + config_path.string());
    }
    out << cfg.dump(2) << '\n';
}

std::string ryujinx_profile_display_name(const std::string& preferred) {
    std::string name;
    name.reserve(std::min<std::size_t>(preferred.size(), 32));
    for (char character : preferred) {
        if (name.size() >= 32) {
            break;
        }
        const auto code = static_cast<unsigned char>(character);
        if (code < 0x20 || code == 0x7f) {
            continue;
        }
        name.push_back(character);
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
        name.pop_back();
    }
    if (name.empty()) {
        return "Player";
    }
    bool capitalize = true;
    for (char& character : name) {
        if (character == ' ' || character == '-' || character == '_') {
            if (character == '_' || character == '-') {
                character = ' ';
            }
            capitalize = true;
            continue;
        }
        if (capitalize && character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - 'a' + 'A');
        } else if (!capitalize && character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
        capitalize = false;
    }
    return name;
}

std::string ryujinx_custom_user_id(const std::string& save_username) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char character : save_username) {
        hash ^= character;
        hash *= 1099511628211ull;
    }
    char buffer[33] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "a5c57ea1%08x%08x%08x",
        static_cast<unsigned>(hash >> 32),
        static_cast<unsigned>(hash),
        static_cast<unsigned>(hash ^ (hash >> 17)));
    return buffer;
}

void ensure_ryujinx_profiles_json(
    const std::filesystem::path& data_root,
    const std::string& save_username,
    const std::string& preferred_display_name) {
    const auto profiles_path = data_root / "system" / "Profiles.json";
    std::filesystem::create_directories(profiles_path.parent_path());

    nlohmann::json cfg = nlohmann::json::object();
    const auto try_load = [&](const std::filesystem::path& path) {
        if (!std::filesystem::is_regular_file(path)) {
            return false;
        }
        try {
            std::ifstream in(path);
            auto parsed = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/true);
            if (!parsed.is_object()) {
                return false;
            }
            cfg = std::move(parsed);
            return true;
        } catch (const nlohmann::json::exception&) {
            return false;
        }
    };

    if (!try_load(profiles_path)) {
        (void)try_load(SwitchSystemDefaults::system_root() / "ryujinx_Profiles.json");
    }

    const auto source = !preferred_display_name.empty() ? preferred_display_name : save_username;
    const auto display_name = ryujinx_profile_display_name(source);
    const auto custom_id = ryujinx_custom_user_id(save_username);
    const auto now = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    if (!cfg.contains("profiles") || !cfg["profiles"].is_array()) {
        cfg["profiles"] = nlohmann::json::array();
    }
    auto& profiles = cfg["profiles"];

    std::string avatar_image;
    for (const auto& entry : profiles) {
        if (entry.is_object() && entry.contains("image") && entry["image"].is_string()) {
            const auto& image = entry["image"].get_ref<const std::string&>();
            if (!image.empty()) {
                avatar_image = image;
                break;
            }
        }
    }

    nlohmann::json* custom = nullptr;
    for (auto& entry : profiles) {
        if (!entry.is_object()) {
            continue;
        }
        if (entry.value("user_id", "") == custom_id) {
            custom = &entry;
            break;
        }
    }

    if (custom == nullptr) {
        profiles.push_back({
            {"user_id", custom_id},
            {"name", display_name},
            {"account_state", "Open"},
            {"online_play_state", "Closed"},
            {"last_modified_timestamp", now},
            {"image", avatar_image},
        });
        custom = &profiles.back();
    } else {
        (*custom)["name"] = display_name;
        (*custom)["last_modified_timestamp"] = now;
        if ((!custom->contains("image") || !(*custom)["image"].is_string() ||
             custom->at("image").get<std::string>().empty()) &&
            !avatar_image.empty()) {
            (*custom)["image"] = avatar_image;
        }
    }

    for (auto& entry : profiles) {
        if (!entry.is_object()) {
            continue;
        }
        const auto id = entry.value("user_id", "");
        if (id == custom_id) {
            entry["account_state"] = "Open";
            entry["name"] = display_name;
            entry["online_play_state"] = "Closed";
        } else {
            entry["account_state"] = "Closed";
        }
    }

    std::ofstream out(profiles_path, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to write Ryujinx Profiles.json: " + profiles_path.string());
    }
    out << cfg.dump(2) << '\n';
    std::cout << "Ryujinx Profiles: Open user \"" << display_name << "\"\n";
}

} // namespace

RyujinxUserProfile RyujinxUserProfileService::prepare(
    const SaveProfile& save_profile,
    bool enable_ldn_mitm,
    int resolution_scale,
    std::string_view profile_display_name,
    std::string_view lan_interface_id,
    std::string_view content_stem,
    std::string_view title_id) {
    RyujinxUserProfile profile;
    profile.xdg_config_home = save_profile.user_directory / "ryujinx" / "xdg-config";
    profile.data_root = profile.xdg_config_home / "Ryujinx";
    profile.keys_directory = profile.data_root / "system";

    std::filesystem::create_directories(profile.data_root / "bis" / "user" / "save");
    std::filesystem::create_directories(profile.keys_directory);
    std::filesystem::create_directories(save_profile.user_directory / "switch" / "saves");
    std::filesystem::create_directories(save_profile.user_directory / "switch" / "addons");

    SwitchSystemDefaults::ensure();
    switch_copy_key_files(SwitchSystemDefaults::keys_directory(), profile.keys_directory);
    if (!std::filesystem::is_regular_file(profile.keys_directory / "prod.keys")) {
        switch_copy_key_files(SwitchPaths::ryujinx_runtime_root() / "keys", profile.keys_directory);
    }
    if (!std::filesystem::is_regular_file(profile.keys_directory / "prod.keys")) {
        switch_copy_key_files(SwitchPaths::yuzu_runtime_root() / "keys", profile.keys_directory);
    }
    if (!std::filesystem::is_regular_file(profile.keys_directory / "prod.keys")) {
        if (const auto source_keys = SwitchSystemDefaults::find_source_keys_dir(); source_keys.has_value()) {
            switch_copy_key_files(*source_keys, profile.keys_directory);
        }
    }

    SwitchSystemDefaults::ensure_ryujinx_firmware(profile.data_root);
    if (!content_stem.empty()) {
        ensure_ryujinx_catalog_addons(save_profile, profile.data_root, content_stem, title_id);
    } else {
        ensure_ryujinx_title_updates(profile.data_root);
    }

    const auto updates_dir = switch_title_updates_directory();
    const std::string lan_iface{lan_interface_id};

    const auto template_config = save_profile.root_directory / "template" / "ryujinx" / "xdg-config" /
        "Ryujinx" / "Config.json";
    if (!std::filesystem::is_regular_file(template_config)) {
        ensure_ryujinx_config(
            template_config,
            /*enable_ldn_mitm=*/true,
            /*resolution_scale=*/1,
            updates_dir,
            lan_iface);
    }

    const auto user_config = profile.data_root / "Config.json";
    if (!std::filesystem::is_regular_file(user_config)) {
        std::error_code ec;
        std::filesystem::copy_file(
            template_config,
            user_config,
            std::filesystem::copy_options::skip_existing,
            ec);
        if (ec) {
            throw std::runtime_error(
                "failed to seed user Ryujinx Config.json from template: " + ec.message());
        }
    }

    ensure_ryujinx_config(user_config, enable_ldn_mitm, resolution_scale, updates_dir, lan_iface);
    const std::string display =
        profile_display_name.empty() ? save_profile.username : std::string(profile_display_name);
    ensure_ryujinx_profiles_json(profile.data_root, save_profile.username, display);
    return profile;
}

} // namespace archstreamer
