#include "host/switch/ryujinx_title_updates.hpp"

#include "host/switch_save_share.hpp"
#include "host/switch/switch_system_defaults.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>
#include <vector>

namespace archstreamer {
namespace {

constexpr std::uint32_t read_le_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
        (static_cast<std::uint32_t>(p[1]) << 8) |
        (static_cast<std::uint32_t>(p[2]) << 16) |
        (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t read_le_u64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(read_le_u32(p)) |
        (static_cast<std::uint64_t>(read_le_u32(p + 4)) << 32);
}

bool extract_pfs0_nsp(
    const std::filesystem::path& nsp_path,
    const std::filesystem::path& destination_dir) {
    std::ifstream in(nsp_path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::uint8_t header[16];
    in.read(reinterpret_cast<char*>(header), 16);
    if (!in || std::memcmp(header, "PFS0", 4) != 0) {
        return false;
    }
    const auto file_count = read_le_u32(header + 4);
    const auto string_table_size = read_le_u32(header + 8);
    if (file_count == 0 || file_count > 512) {
        return false;
    }

    std::vector<std::uint8_t> entries(static_cast<std::size_t>(file_count) * 24);
    in.read(reinterpret_cast<char*>(entries.data()), static_cast<std::streamsize>(entries.size()));
    if (!in) {
        return false;
    }
    std::vector<char> string_table(string_table_size);
    if (string_table_size > 0) {
        in.read(string_table.data(), static_cast<std::streamsize>(string_table_size));
        if (!in) {
            return false;
        }
    }

    const auto data_start =
        16 + static_cast<std::uint64_t>(file_count) * 24 + string_table_size;
    std::filesystem::create_directories(destination_dir);

    int extracted = 0;
    for (std::uint32_t i = 0; i < file_count; ++i) {
        const auto* entry = entries.data() + static_cast<std::size_t>(i) * 24;
        const auto offset = read_le_u64(entry);
        const auto size = read_le_u64(entry + 8);
        const auto name_offset = read_le_u32(entry + 16);
        if (name_offset >= string_table_size) {
            continue;
        }
        std::string name(string_table.data() + name_offset);
        if (name.empty()) {
            continue;
        }
        const auto dest = destination_dir / name;
        if (std::filesystem::is_regular_file(dest) &&
            std::filesystem::file_size(dest) == size) {
            continue;
        }
        in.clear();
        in.seekg(static_cast<std::streamoff>(data_start + offset));
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (!out) {
            continue;
        }
        std::vector<char> buffer(static_cast<std::size_t>(std::min<std::uint64_t>(size, 1 << 20)));
        std::uint64_t remaining = size;
        while (remaining > 0) {
            const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
            in.read(buffer.data(), static_cast<std::streamsize>(chunk));
            if (!in) {
                break;
            }
            out.write(buffer.data(), static_cast<std::streamsize>(chunk));
            remaining -= chunk;
        }
        if (remaining == 0) {
            ++extracted;
        }
    }
    return extracted > 0 || file_count > 0;
}

std::string to_lower_copy(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

bool stem_looks_versioned(std::string_view content_stem) {
    static const std::regex version_token(R"((?:^|[\s_-])v?\d+\.\d+(?:\.\d+)?(?:$|[\s_-]))", std::regex::icase);
    return std::regex_search(std::string(content_stem), version_token);
}

std::string base_name_for_matching(std::string_view content_stem) {
    std::string base = to_lower_copy(content_stem);
    static const std::regex version_suffix(R"((?:\s+|[_-])v?\d+(?:\.\d+){1,3}\s*$)", std::regex::icase);
    base = std::regex_replace(base, version_suffix, "");
    while (!base.empty() && base.back() == ' ') {
        base.pop_back();
    }
    return base.empty() ? to_lower_copy(content_stem) : base;
}

std::vector<std::string> seed_nsp_names_for_stem(std::string_view content_stem) {
    if (!stem_looks_versioned(content_stem)) {
        return {};
    }
    const auto base = base_name_for_matching(content_stem);
    const auto updates_dir = switch_title_updates_directory();
    std::vector<std::string> names;
    std::error_code ec;
    if (!std::filesystem::is_directory(updates_dir, ec)) {
        return names;
    }
    for (const auto& entry : std::filesystem::directory_iterator(updates_dir, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto ext = entry.path().extension().string();
        if (ext != ".nsp" && ext != ".NSP") {
            continue;
        }
        const auto filename = entry.path().filename().string();
        const auto lower = to_lower_copy(filename);
        if (lower.find(base) == std::string::npos) {
            continue;
        }
        // UPD or DLC packs for this title.
        if (lower.find("[upd]") == std::string::npos &&
            lower.find("[dlc]") == std::string::npos &&
            lower.find("upd]") == std::string::npos &&
            lower.find("dlc]") == std::string::npos) {
            continue;
        }
        names.push_back(filename);
    }
    std::sort(names.begin(), names.end());
    return names;
}

void ensure_manifest(const std::filesystem::path& addon_dir, std::string_view content_stem) {
    const auto manifest_path = addon_dir / "manifest.json";
    std::error_code ec;
    if (std::filesystem::is_regular_file(manifest_path, ec)) {
        return;
    }
    nlohmann::json doc;
    doc["content_stem"] = std::string(content_stem);
    doc["nsps"] = seed_nsp_names_for_stem(content_stem);
    doc["seeded"] = true;
    std::filesystem::create_directories(addon_dir, ec);
    std::ofstream out(manifest_path, std::ios::trunc);
    out << doc.dump(2) << '\n';
    std::cout
        << "switch addons: seeded manifest for \"" << content_stem << "\" with "
        << doc["nsps"].size() << " NSP(s)\n";
}

std::vector<std::filesystem::path> nsp_paths_from_manifest(const std::filesystem::path& addon_dir) {
    const auto manifest_path = addon_dir / "manifest.json";
    std::ifstream in(manifest_path);
    if (!in) {
        return {};
    }
    nlohmann::json doc;
    try {
        in >> doc;
    } catch (...) {
        return {};
    }
    const auto updates_dir = switch_title_updates_directory();
    std::vector<std::filesystem::path> paths;
    if (!doc.contains("nsps") || !doc["nsps"].is_array()) {
        return paths;
    }
    for (const auto& item : doc["nsps"]) {
        if (!item.is_string()) {
            continue;
        }
        const auto name = item.get<std::string>();
        if (name.empty()) {
            continue;
        }
        std::filesystem::path candidate(name);
        if (!candidate.is_absolute()) {
            candidate = updates_dir / name;
        }
        if (std::filesystem::is_regular_file(candidate)) {
            paths.push_back(candidate);
        } else {
            std::cerr << "switch addons: missing NSP " << candidate << '\n';
        }
    }
    return paths;
}

bool replace_registered_with_symlink(
    const std::filesystem::path& ryujinx_registered,
    const std::filesystem::path& addon_registered) {
    std::error_code ec;
    std::filesystem::create_directories(addon_registered, ec);
    std::filesystem::create_directories(ryujinx_registered.parent_path(), ec);

    if (std::filesystem::is_symlink(ryujinx_registered, ec)) {
        const auto current = std::filesystem::read_symlink(ryujinx_registered, ec);
        if (!ec) {
            auto resolved = current;
            if (!resolved.is_absolute()) {
                resolved = ryujinx_registered.parent_path() / resolved;
            }
            if (resolved == addon_registered) {
                return true;
            }
        }
        std::filesystem::remove(ryujinx_registered, ec);
    } else if (std::filesystem::exists(ryujinx_registered, ec)) {
        // Preserve previous real registered tree once as backup, then replace.
        const auto backup = ryujinx_registered.parent_path() / "registered.archstreamer-bak";
        if (!std::filesystem::exists(backup, ec)) {
            std::filesystem::rename(ryujinx_registered, backup, ec);
            if (ec) {
                std::filesystem::remove_all(ryujinx_registered, ec);
            }
        } else {
            std::filesystem::remove_all(ryujinx_registered, ec);
        }
    }
    std::filesystem::create_directory_symlink(addon_registered, ryujinx_registered, ec);
    if (ec) {
        std::cerr
            << "switch addons: failed to link registered -> " << addon_registered
            << ": " << ec.message() << '\n';
        return false;
    }
    return true;
}

} // namespace

std::filesystem::path switch_title_updates_directory() {
    if (const char* env = std::getenv("ARCHSTREAMER_SWITCH_UPDATES");
        env != nullptr && *env != '\0') {
        return env;
    }
    const std::filesystem::path gaming{"/mnt/Internal_SSD/Gaming/ROMS/SwitchUpdates"};
    if (std::filesystem::is_directory(gaming)) {
        return gaming;
    }
    return SwitchSystemDefaults::system_root() / "updates";
}

void ensure_ryujinx_catalog_addons(
    const SaveProfile& save_profile,
    const std::filesystem::path& ryujinx_data_root,
    std::string_view content_stem,
    std::string_view title_id) {
    (void)title_id;
    if (content_stem.empty()) {
        return;
    }
    const auto addon_dir = catalog_switch_addon_directory(save_profile, content_stem);
    const auto registered = addon_dir / "registered";
    ensure_manifest(addon_dir, content_stem);
    std::filesystem::create_directories(registered);

    int unpacked = 0;
    const auto nsps = nsp_paths_from_manifest(addon_dir);
    for (const auto& nsp : nsps) {
        if (extract_pfs0_nsp(nsp, registered)) {
            ++unpacked;
        }
    }

    const auto ryu_registered = ryujinx_data_root / "bis" / "user" / "Contents" / "registered";
    if (replace_registered_with_symlink(ryu_registered, registered)) {
        std::cout
            << "Ryujinx catalog addons: \"" << content_stem << "\" → " << registered
            << " (" << nsps.size() << " NSP(s), " << unpacked << " extracted)\n";
    }
}

void ensure_ryujinx_title_updates(const std::filesystem::path& ryujinx_data_root) {
    // Kept for tooling/back-compat; catalog launches use ensure_ryujinx_catalog_addons.
    const auto updates_dir = switch_title_updates_directory();
    if (!std::filesystem::is_directory(updates_dir)) {
        return;
    }

    const auto registered = ryujinx_data_root / "bis" / "user" / "Contents" / "registered";
    std::error_code ec;
    if (std::filesystem::is_symlink(registered, ec)) {
        // Do not dump every NSP into a catalog-specific registered symlink.
        return;
    }
    std::filesystem::create_directories(registered);

    int nsp_count = 0;
    int unpacked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(updates_dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto ext = entry.path().extension().string();
        if (ext != ".nsp" && ext != ".NSP") {
            continue;
        }
        ++nsp_count;
        if (extract_pfs0_nsp(entry.path(), registered)) {
            ++unpacked;
        }
    }
    if (nsp_count > 0) {
        std::cout
            << "Ryujinx title updates: scanned " << nsp_count << " NSP(s) from " << updates_dir
            << " → " << registered << " (" << unpacked << " unpacked/linked)\n";
    }
}

} // namespace archstreamer
