#include "host/switch/switch_system_defaults.hpp"

#include "host/switch/default_switch_paths.hpp"
#include "host/switch/switch_fs.hpp"

#include <iostream>
#include <system_error>
#include <vector>

namespace archstreamer {

std::filesystem::path SwitchSystemDefaults::system_root() {
    return SwitchPaths::archstreamer_data_root() / "system" / "switch";
}

std::filesystem::path SwitchSystemDefaults::keys_directory() {
    return system_root() / "keys";
}

std::filesystem::path SwitchSystemDefaults::managed_firmware_registered() {
    return system_root() / "firmware" / "registered";
}

std::optional<std::filesystem::path> SwitchSystemDefaults::find_source_keys_dir() {
    for (const auto& candidate : SwitchPaths::keys_source_candidates()) {
        if (std::filesystem::is_regular_file(candidate / "prod.keys")) {
            return candidate;
        }
    }
    return std::nullopt;
}

void SwitchSystemDefaults::ensure() {
    const auto keys_dir = keys_directory();
    std::filesystem::create_directories(keys_dir);
    if (!std::filesystem::is_regular_file(keys_dir / "prod.keys")) {
        std::vector<std::filesystem::path> sources;
        sources.push_back(keys_dir);
        sources.push_back(SwitchPaths::ryujinx_runtime_root() / "keys");
        sources.push_back(SwitchPaths::yuzu_runtime_root() / "keys");
        if (const auto source = find_source_keys_dir(); source.has_value()) {
            sources.push_back(*source);
        }
        for (const auto& source : sources) {
            if (source == keys_dir) {
                continue;
            }
            switch_copy_key_files(source, keys_dir);
            if (std::filesystem::is_regular_file(keys_dir / "prod.keys")) {
                std::cout << "Switch keys: installed into " << keys_dir << " (from " << source << ")\n";
                break;
            }
        }
    }

    const auto profiles_template = system_root() / "ryujinx_Profiles.json";
    if (!std::filesystem::is_regular_file(profiles_template)) {
        for (const auto& source : SwitchPaths::profiles_template_source_candidates()) {
            if (!std::filesystem::is_regular_file(source)) {
                continue;
            }
            std::error_code ec;
            std::filesystem::create_directories(profiles_template.parent_path(), ec);
            std::filesystem::copy_file(
                source,
                profiles_template,
                std::filesystem::copy_options::skip_existing,
                ec);
            if (std::filesystem::is_regular_file(profiles_template)) {
                std::cout << "Ryujinx profile template: seeded from " << source << '\n';
                break;
            }
        }
    }

    const auto firmware = managed_firmware_registered();
    if (switch_directory_has_entries(firmware)) {
        return;
    }
    for (const auto& source : SwitchPaths::firmware_source_candidates(firmware)) {
        if (source == firmware || !switch_directory_has_entries(source)) {
            continue;
        }
        std::error_code ec;
        if (std::filesystem::equivalent(source, firmware, ec)) {
            continue;
        }
        std::cout << "Switch firmware: installing into " << firmware << " (from " << source << ")\n";
        switch_copy_directory_recursive_skip_existing(source, firmware);
        if (switch_directory_has_entries(firmware)) {
            return;
        }
    }
}

std::filesystem::path SwitchSystemDefaults::ensure_managed_firmware() {
    ensure();
    return managed_firmware_registered();
}

void SwitchSystemDefaults::ensure_ryujinx_firmware(const std::filesystem::path& data_root) {
    const auto profile_registered = data_root / "bis" / "system" / "Contents" / "registered";
    if (switch_directory_has_entries(profile_registered)) {
        return;
    }

    const auto managed = ensure_managed_firmware();
    if (!switch_directory_has_entries(managed)) {
        std::cout << "Warning: Switch firmware not found under " << managed
                  << ". Place firmware NCAs there (or bootstrap once from a desktop Ryujinx "
                     "install).\n";
        std::filesystem::create_directories(profile_registered);
        return;
    }

    SwitchPaths::bind_ryujinx_firmware(managed, profile_registered);
}

} // namespace archstreamer
