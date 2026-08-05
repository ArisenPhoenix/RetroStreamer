#include "host/switch/ryujinx_title_updates.hpp"

#include "common/dlc_paths.hpp"
#include "host/switch_save_share.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
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

/** Base title for NSP/addon matching: strip edge " (1.3.2)" suffix only. */
std::string base_name_for_matching(std::string_view content_stem) {
    std::string base = to_lower_copy(content_stem);
    static const std::regex paren_suffix(R"(\s+\([^)]*\)\s*$)");
    base = std::regex_replace(base, paren_suffix, "");
    while (!base.empty() && base.back() == ' ') {
        base.pop_back();
    }
    return base.empty() ? to_lower_copy(content_stem) : base;
}

bool filename_is_upd_or_dlc(std::string_view filename_lower) {
    return filename_lower.find("[upd]") != std::string::npos ||
        filename_lower.find("[dlc]") != std::string::npos ||
        filename_lower.find("upd]") != std::string::npos ||
        filename_lower.find("dlc]") != std::string::npos;
}

bool directory_has_nca_payload(const std::filesystem::path& registered) {
    std::error_code ec;
    if (!std::filesystem::is_directory(registered, ec)) {
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(registered, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const auto ext = to_lower_copy(entry.path().extension().string());
        if (ext == ".nca" || ext.find(".nca") != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> list_nsp_basenames_in_dir(
    const std::filesystem::path& dir,
    std::string_view base_filter) {
    std::vector<std::string> names;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return names;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto ext = entry.path().extension().string();
        if (ext != ".nsp" && ext != ".NSP") {
            continue;
        }
        const auto filename = entry.path().filename().string();
        const auto lower = to_lower_copy(filename);
        if (!base_filter.empty() && lower.find(base_filter) == std::string::npos) {
            continue;
        }
        if (!filename_is_upd_or_dlc(lower)) {
            continue;
        }
        names.push_back(filename);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> seed_nsp_names_for_stem(
    const std::filesystem::path& addon_dir,
    std::string_view content_stem) {
    const auto base = base_name_for_matching(content_stem);
    if (base.empty()) {
        return {};
    }
    auto names = list_nsp_basenames_in_dir(addon_dir, base);
    if (!names.empty()) {
        return names;
    }
    // Fall back to legacy flat SwitchUpdates pack folder.
    return list_nsp_basenames_in_dir(legacy_switch_updates_directory(), base);
}

void migrate_nsps_into_addon_dir(
    const std::filesystem::path& addon_dir,
    const std::vector<std::string>& nsp_names) {
    const auto legacy = legacy_switch_updates_directory();
    std::error_code ec;
    std::filesystem::create_directories(addon_dir, ec);
    for (const auto& name : nsp_names) {
        const auto dest = addon_dir / name;
        if (std::filesystem::is_regular_file(dest, ec)) {
            continue;
        }
        const auto src = legacy / name;
        if (!std::filesystem::is_regular_file(src, ec)) {
            continue;
        }
        std::filesystem::rename(src, dest, ec);
        if (ec) {
            ec.clear();
            std::filesystem::copy_file(src, dest, std::filesystem::copy_options::skip_existing, ec);
            if (!ec) {
                std::filesystem::remove(src, ec);
            }
        }
        if (!ec) {
            std::cout << "switch DLC: migrated NSP → " << dest << '\n';
        }
    }
}

void migrate_registered_from_legacy_user_addons(
    const SaveProfile& save_profile,
    std::string_view content_stem,
    const std::filesystem::path& addon_registered) {
    if (directory_has_nca_payload(addon_registered)) {
        return;
    }
    std::error_code ec;
    std::vector<std::filesystem::path> candidates;
    const auto base = base_name_for_matching(content_stem);

    auto consider_registered = [&](const std::filesystem::path& cand) {
        if (cand == addon_registered) {
            return;
        }
        if (directory_has_nca_payload(cand)) {
            candidates.push_back(cand);
        }
    };

    const auto own =
        save_profile.user_directory / "switch" / "addons" / std::string(content_stem) / "registered";
    consider_registered(own);

    auto scan_addons_root = [&](const std::filesystem::path& addons_root) {
        if (!std::filesystem::is_directory(addons_root, ec)) {
            return;
        }
        for (const auto& stem_entry : std::filesystem::directory_iterator(addons_root, ec)) {
            if (!stem_entry.is_directory(ec)) {
                continue;
            }
            const auto stem_name = stem_entry.path().filename().string();
            if (stem_name.empty()) {
                continue;
            }
            // Exact stem or same base (Pokemon Shield ← Pokemon Shield 1.3.2).
            if (stem_name != content_stem
                && (base.empty() || base_name_for_matching(stem_name) != base)) {
                continue;
            }
            consider_registered(stem_entry.path() / "registered");
        }
    };

    scan_addons_root(save_profile.user_directory / "switch" / "addons");

    const auto save_root = save_profile.root_directory;
    if (std::filesystem::is_directory(save_root, ec)) {
        for (const auto& user_entry : std::filesystem::directory_iterator(save_root, ec)) {
            if (!user_entry.is_directory(ec)) {
                continue;
            }
            const auto username = user_entry.path().filename().string();
            if (username.empty() || username == "template" || username.front() == '.') {
                continue;
            }
            scan_addons_root(user_entry.path() / "switch" / "addons");
        }
    }

    // Global DLC/Switch/<stem>/registered siblings.
    scan_addons_root(switch_title_updates_directory());

    if (candidates.empty()) {
        return;
    }

    // Prefer the largest tree (most complete unpack).
    auto count_files = [](const std::filesystem::path& dir) {
        std::error_code iec;
        std::size_t n = 0;
        for (const auto& e : std::filesystem::directory_iterator(dir, iec)) {
            if (e.is_regular_file(iec)) {
                ++n;
            }
        }
        return n;
    };
    std::sort(candidates.begin(), candidates.end(), [&](const auto& a, const auto& b) {
        return count_files(a) > count_files(b);
    });

    const auto& source = candidates.front();
    std::filesystem::create_directories(addon_registered.parent_path(), ec);
    if (std::filesystem::exists(addon_registered, ec)) {
        std::filesystem::remove_all(addon_registered, ec);
    }
    std::filesystem::rename(source, addon_registered, ec);
    if (ec) {
        ec.clear();
        std::filesystem::copy(
            source,
            addon_registered,
            std::filesystem::copy_options::recursive | std::filesystem::copy_options::skip_existing,
            ec);
        if (!ec) {
            std::filesystem::remove_all(source, ec);
        }
    }
    if (!ec) {
        std::cout
            << "switch DLC: migrated registered NCAs from " << source << " → " << addon_registered
            << '\n';
        // Drop empty leftover per-user addon dirs (best effort).
        for (const auto& cand : candidates) {
            if (cand == addon_registered) {
                continue;
            }
            const auto parent = cand.parent_path();
            std::filesystem::remove(cand, ec);
            std::filesystem::remove(parent / "manifest.json", ec);
            std::filesystem::remove(parent, ec);
        }
    }
}

void ensure_manifest(
    const std::filesystem::path& addon_dir,
    std::string_view game_id,
    std::string_view content_stem) {
    const auto manifest_path = addon_dir / "manifest.json";
    std::error_code ec;
    if (std::filesystem::is_regular_file(manifest_path, ec)) {
        return;
    }
    const auto match_key = !content_stem.empty() ? content_stem : game_id;
    auto names = seed_nsp_names_for_stem(addon_dir, match_key);
    migrate_nsps_into_addon_dir(addon_dir, names);
    // Re-list after migration so manifest only stores basenames in the game folder.
    names = list_nsp_basenames_in_dir(addon_dir, base_name_for_matching(match_key));
    if (names.empty()) {
        names = seed_nsp_names_for_stem(addon_dir, match_key);
    }

    nlohmann::json doc;
    doc["game_id"] = std::string(game_id);
    if (!content_stem.empty()) {
        doc["content_stem"] = std::string(content_stem);
    }
    doc["nsps"] = names;
    doc["seeded"] = true;
    std::filesystem::create_directories(addon_dir, ec);
    std::ofstream out(manifest_path, std::ios::trunc);
    out << doc.dump(2) << '\n';
    std::cout
        << "switch DLC: seeded manifest for game_id \"" << game_id << "\" with " << names.size()
        << " NSP(s) at " << addon_dir << '\n';
}

void migrate_legacy_stem_folder_into_game_id(
    std::string_view content_stem,
    const std::filesystem::path& addon_dir) {
    if (content_stem.empty() || addon_dir.empty()) {
        return;
    }
    const auto legacy = catalog_dlc_legacy_stem_directory(resolve_dlc_root(), "switch", content_stem);
    if (legacy.empty() || legacy == addon_dir) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(legacy, ec) || ec) {
        return;
    }
    if (std::filesystem::exists(addon_dir, ec) && !ec) {
        // Prefer merging registered/NSPs via existing registered migrate + manifest seed.
        return;
    }
    std::filesystem::create_directories(addon_dir.parent_path(), ec);
    std::filesystem::rename(legacy, addon_dir, ec);
    if (!ec) {
        std::cout << "switch DLC: migrated stem folder " << legacy << " → " << addon_dir << '\n';
    }
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
    const auto legacy = legacy_switch_updates_directory();
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
            candidate = addon_dir / name;
            if (!std::filesystem::is_regular_file(candidate)) {
                candidate = legacy / name;
            }
        }
        if (std::filesystem::is_regular_file(candidate)) {
            // Keep packs nested with the game when resolved from legacy.
            if (candidate.parent_path() != addon_dir) {
                migrate_nsps_into_addon_dir(addon_dir, {name});
                const auto nested = addon_dir / name;
                if (std::filesystem::is_regular_file(nested)) {
                    candidate = nested;
                }
            }
            paths.push_back(candidate);
        } else {
            std::cerr << "switch DLC: missing NSP " << name << " under " << addon_dir << '\n';
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
            << "switch DLC: failed to link registered -> " << addon_registered << ": "
            << ec.message() << '\n';
        return false;
    }
    return true;
}

void collect_nsp_files_recursive(
    const std::filesystem::path& root,
    std::vector<std::filesystem::path>& out) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto ext = entry.path().extension().string();
        if (ext == ".nsp" || ext == ".NSP") {
            out.push_back(entry.path());
        }
    }
}

} // namespace

std::filesystem::path switch_title_updates_directory() {
    // System folder under global DLC (per-game packs nest underneath).
    return resolve_dlc_root() / dlc_system_folder_name("switch");
}

void ensure_ryujinx_catalog_addons(
    const SaveProfile& save_profile,
    const std::filesystem::path& ryujinx_data_root,
    std::string_view game_id,
    std::string_view content_stem,
    std::string_view title_id) {
    (void)title_id;
    if (game_id.empty()) {
        return;
    }
    const auto addon_dir = switch_dlc_game_directory(game_id);
    if (addon_dir.empty()) {
        return;
    }
    migrate_legacy_stem_folder_into_game_id(content_stem, addon_dir);
    const auto registered = addon_dir / "registered";
    ensure_manifest(addon_dir, game_id, content_stem);
    if (!content_stem.empty()) {
        migrate_registered_from_legacy_user_addons(save_profile, content_stem, registered);
    }
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
            << "Ryujinx catalog DLC: game_id \"" << game_id << "\" → " << registered << " ("
            << nsps.size() << " NSP(s), " << unpacked << " extracted)\n";
    }
}

void ensure_ryujinx_title_updates(const std::filesystem::path& ryujinx_data_root) {
    // Kept for tooling/back-compat; catalog launches use ensure_ryujinx_catalog_addons.
    std::vector<std::filesystem::path> nsp_roots;
    nsp_roots.push_back(switch_title_updates_directory());
    const auto legacy = legacy_switch_updates_directory();
    if (legacy != nsp_roots.front()) {
        nsp_roots.push_back(legacy);
    }

    const auto registered = ryujinx_data_root / "bis" / "user" / "Contents" / "registered";
    std::error_code ec;
    if (std::filesystem::is_symlink(registered, ec)) {
        return;
    }
    std::filesystem::create_directories(registered);

    std::vector<std::filesystem::path> nsps;
    for (const auto& root : nsp_roots) {
        collect_nsp_files_recursive(root, nsps);
    }
    int unpacked = 0;
    for (const auto& nsp : nsps) {
        if (extract_pfs0_nsp(nsp, registered)) {
            ++unpacked;
        }
    }
    if (!nsps.empty()) {
        std::cout
            << "Ryujinx title updates: scanned " << nsps.size() << " NSP(s) → " << registered
            << " (" << unpacked << " unpacked/linked)\n";
    }
}

} // namespace archstreamer
