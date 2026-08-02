#include "host/switch_save_share.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace archstreamer {
namespace {

bool looks_like_title_id(std::string_view value) {
    if (value.size() != 16) {
        return false;
    }
    for (char ch : value) {
        if (!std::isxdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    return value.rfind("0100", 0) == 0;
}

std::optional<std::uint64_t> read_le_u64(const std::filesystem::path& path, std::size_t offset) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    in.seekg(static_cast<std::streamoff>(offset));
    std::uint64_t value = 0;
    in.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!in) {
        return std::nullopt;
    }
    return value;
}

std::string title_id_from_u64(std::uint64_t value) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(value));
    return normalize_switch_title_id(buf);
}

bool source_should_replace_dest(
    const std::filesystem::path& src,
    const std::filesystem::path& dst) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(src, ec)) {
        return false;
    }
    if (!std::filesystem::exists(dst, ec) || !std::filesystem::is_regular_file(dst, ec)) {
        return true;
    }
    const auto src_time = std::filesystem::last_write_time(src, ec);
    if (ec) {
        return false;
    }
    const auto dst_time = std::filesystem::last_write_time(dst, ec);
    if (ec) {
        return true;
    }
    if (src_time > dst_time) {
        return true;
    }
    if (src_time < dst_time) {
        return false;
    }
    // Equal mtime: prefer the larger file (more complete write).
    const auto src_size = std::filesystem::file_size(src, ec);
    if (ec) {
        return false;
    }
    const auto dst_size = std::filesystem::file_size(dst, ec);
    if (ec) {
        return true;
    }
    return src_size > dst_size;
}

bool copy_file_overwrite_preserve_mtime(
    const std::filesystem::path& src,
    const std::filesystem::path& dst) {
    std::error_code ec;
    std::filesystem::create_directories(dst.parent_path(), ec);
    std::filesystem::copy_file(
        src,
        dst,
        std::filesystem::copy_options::overwrite_existing,
        ec);
    if (ec) {
        std::cerr << "switch save share: copy failed " << src << " -> " << dst
                  << ": " << ec.message() << '\n';
        return false;
    }
    const auto src_time = std::filesystem::last_write_time(src, ec);
    if (!ec) {
        std::filesystem::last_write_time(dst, src_time, ec);
    }
    return true;
}

/** Bidirectional newer-wins mirror of regular files between two directories. */
int mirror_files_newer_wins(
    const std::filesystem::path& left,
    const std::filesystem::path& right) {
    std::error_code ec;
    std::filesystem::create_directories(left, ec);
    std::filesystem::create_directories(right, ec);

    std::unordered_set<std::string> names;
    for (const auto* root : {&left, &right}) {
        if (!std::filesystem::is_directory(*root, ec)) {
            continue;
        }
        for (const auto& entry : std::filesystem::directory_iterator(*root, ec)) {
            if (ec) {
                break;
            }
            if (!entry.is_regular_file()) {
                continue;
            }
            names.insert(entry.path().filename().string());
        }
    }

    int changes = 0;
    for (const auto& name : names) {
        const auto left_path = left / name;
        const auto right_path = right / name;
        const bool left_ok = std::filesystem::is_regular_file(left_path, ec);
        const bool right_ok = std::filesystem::is_regular_file(right_path, ec);
        if (left_ok && right_ok) {
            if (source_should_replace_dest(left_path, right_path)) {
                if (copy_file_overwrite_preserve_mtime(left_path, right_path)) {
                    ++changes;
                }
            } else if (source_should_replace_dest(right_path, left_path)) {
                if (copy_file_overwrite_preserve_mtime(right_path, left_path)) {
                    ++changes;
                }
            }
        } else if (left_ok) {
            if (copy_file_overwrite_preserve_mtime(left_path, right_path)) {
                ++changes;
            }
        } else if (right_ok) {
            if (copy_file_overwrite_preserve_mtime(right_path, left_path)) {
                ++changes;
            }
        }
    }
    return changes;
}

/**
 * If leaf is a directory symlink (legacy ArchStreamer layout), replace it with a
 * real directory containing copies of the target's files. LibHac Commit must be
 * able to delete/recreate this path.
 */
bool materialize_directory_if_symlink(const std::filesystem::path& leaf) {
    std::error_code ec;
    if (!std::filesystem::is_symlink(leaf, ec)) {
        if (!std::filesystem::exists(leaf, ec)) {
            std::filesystem::create_directories(leaf, ec);
        }
        return !ec || std::filesystem::is_directory(leaf, ec);
    }

    auto target = std::filesystem::read_symlink(leaf, ec);
    if (ec) {
        std::cerr << "switch save share: read_symlink failed " << leaf
                  << ": " << ec.message() << '\n';
        return false;
    }
    if (!target.is_absolute()) {
        target = leaf.parent_path() / target;
    }

    const auto staging =
        leaf.parent_path() / (leaf.filename().string() + ".archstreamer-materialize");
    std::filesystem::remove_all(staging, ec);
    std::filesystem::create_directories(staging, ec);
    if (ec) {
        std::cerr << "switch save share: staging mkdir failed " << staging
                  << ": " << ec.message() << '\n';
        return false;
    }

    if (std::filesystem::is_directory(target, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(target, ec)) {
            if (ec) {
                break;
            }
            if (!entry.is_regular_file()) {
                continue;
            }
            copy_file_overwrite_preserve_mtime(entry.path(), staging / entry.path().filename());
        }
    }

    std::filesystem::remove(leaf, ec); // symlink only
    if (ec) {
        std::cerr << "switch save share: remove symlink failed " << leaf
                  << ": " << ec.message() << '\n';
        std::filesystem::remove_all(staging, ec);
        return false;
    }
    std::filesystem::rename(staging, leaf, ec);
    if (ec) {
        std::cerr << "switch save share: materialize rename failed " << staging
                  << " -> " << leaf << ": " << ec.message() << '\n';
        return false;
    }
    std::cerr << "switch save share: materialized journal dir " << leaf
              << " (was symlink -> " << target << ")\n";
    return true;
}

/**
 * Replace path with a symlink to target. Any existing real directory is
 * newer-wins merged into target first so files are never discarded.
 */
bool replace_with_symlink(const std::filesystem::path& link_path, const std::filesystem::path& target) {
    std::error_code ec;
    std::filesystem::create_directories(target, ec);

    if (std::filesystem::is_symlink(link_path, ec)) {
        const auto current = std::filesystem::read_symlink(link_path, ec);
        if (!ec && current == target) {
            return true;
        }
        std::filesystem::remove(link_path, ec);
    } else if (std::filesystem::exists(link_path, ec)) {
        if (std::filesystem::is_directory(link_path, ec)) {
            mirror_files_newer_wins(link_path, target);
            std::filesystem::remove_all(link_path, ec);
        } else {
            std::filesystem::remove(link_path, ec);
        }
    }
    std::filesystem::create_directories(link_path.parent_path(), ec);
    std::filesystem::create_directory_symlink(target, link_path, ec);
    if (ec) {
        std::cerr << "switch save share: symlink failed " << link_path << " -> " << target
                  << ": " << ec.message() << '\n';
        return false;
    }
    return true;
}

std::filesystem::path yuzu_title_save_directory(
    const SaveProfile& profile,
    std::string_view title_id) {
    const auto nand_save =
        profile.user_directory / "yuzu" / "xdg-data" / "yuzu" / "nand" / "user" / "save" /
        "0000000000000000";
    if (!std::filesystem::is_directory(nand_save)) {
        return {};
    }
    const auto want = normalize_switch_title_id(title_id);
    for (const auto& user_dir : std::filesystem::directory_iterator(nand_save)) {
        if (!user_dir.is_directory()) {
            continue;
        }
        const auto title_dir = user_dir.path() / want;
        if (std::filesystem::exists(title_dir) || std::filesystem::is_symlink(title_dir)) {
            return title_dir;
        }
        for (const auto& title : std::filesystem::directory_iterator(user_dir.path())) {
            if (!title.is_directory() && !title.is_symlink()) {
                continue;
            }
            if (normalize_switch_title_id(title.path().filename().string()) == want) {
                return title.path();
            }
        }
    }
    for (const auto& user_dir : std::filesystem::directory_iterator(nand_save)) {
        if (user_dir.is_directory()) {
            return user_dir.path() / want;
        }
    }
    const auto fallback = nand_save / "00000000000000000000000000000000" / want;
    return fallback;
}

} // namespace

std::string normalize_switch_title_id(std::string_view title_id) {
    std::string out;
    out.reserve(title_id.size());
    for (char ch : title_id) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

std::filesystem::path canonical_switch_save_directory(
    const SaveProfile& profile,
    std::string_view title_id) {
    return profile.user_directory / "switch" / "saves" / normalize_switch_title_id(title_id);
}

std::filesystem::path ensure_canonical_switch_save(
    const SaveProfile& profile,
    std::string_view title_id) {
    const auto canon = canonical_switch_save_directory(profile, title_id);
    std::filesystem::create_directories(canon);

    const auto yuzu_dir = yuzu_title_save_directory(profile, title_id);
    if (!yuzu_dir.empty() && std::filesystem::is_directory(yuzu_dir) &&
        !std::filesystem::is_symlink(yuzu_dir)) {
        mirror_files_newer_wins(yuzu_dir, canon);
    }
    return canon;
}

bool link_yuzu_save_to_canonical(const SaveProfile& profile, std::string_view title_id) {
    const auto canon = ensure_canonical_switch_save(profile, title_id);
    auto yuzu_dir = yuzu_title_save_directory(profile, title_id);
    if (yuzu_dir.empty()) {
        return false;
    }
    std::filesystem::create_directories(yuzu_dir.parent_path());
    return replace_with_symlink(yuzu_dir, canon);
}

int mirror_ryujinx_saves_with_canonical(
    const std::filesystem::path& ryujinx_bis_user_save,
    const SaveProfile& profile,
    std::string_view title_id) {
    if (!std::filesystem::is_directory(ryujinx_bis_user_save)) {
        return 0;
    }
    const auto want = normalize_switch_title_id(title_id);
    const auto canon = ensure_canonical_switch_save(profile, title_id);
    int mirrored = 0;
    for (const auto& entry : std::filesystem::directory_iterator(ryujinx_bis_user_save)) {
        if (!entry.is_directory()) {
            continue;
        }
        const auto extra = entry.path() / "ExtraData0";
        const auto title_word = read_le_u64(extra, 0);
        if (!title_word.has_value()) {
            continue;
        }
        if (title_id_from_u64(*title_word) != want) {
            continue;
        }
        // ExtraData0[8] == 1 → account/user save (the one with main/backup).
        const auto kind = read_le_u64(extra, 8);
        if (!kind.has_value() || (*kind & 0xffull) != 1ull) {
            continue;
        }

        const auto slot0 = entry.path() / "0";
        const auto slot1 = entry.path() / "1";
        if (!materialize_directory_if_symlink(slot0)) {
            continue;
        }
        if (std::filesystem::is_symlink(slot1)) {
            materialize_directory_if_symlink(slot1);
        }
        std::error_code ec;
        std::filesystem::create_directories(slot0, ec);
        std::filesystem::create_directories(slot1, ec);

        // Working journal may be ahead of committed after a crash; promote first.
        mirror_files_newer_wins(slot0, slot1);
        mirror_files_newer_wins(slot0, canon);
        // Keep working aligned with committed for the next EnsureSaveData.
        mirror_files_newer_wins(slot0, slot1);
        ++mirrored;
    }
    return mirrored;
}

std::vector<std::string> sync_switch_shared_saves(
    const SaveProfile& profile,
    const std::filesystem::path& yuzu_nand_user_save,
    const std::filesystem::path& ryujinx_bis_user_save) {
    std::unordered_set<std::string> titles;

    if (std::filesystem::is_directory(yuzu_nand_user_save)) {
        for (const auto& user_dir : std::filesystem::directory_iterator(yuzu_nand_user_save)) {
            if (!user_dir.is_directory()) {
                continue;
            }
            for (const auto& title : std::filesystem::directory_iterator(user_dir.path())) {
                const auto name = normalize_switch_title_id(title.path().filename().string());
                if (looks_like_title_id(name)) {
                    titles.insert(name);
                }
            }
        }
    }

    if (std::filesystem::is_directory(ryujinx_bis_user_save)) {
        for (const auto& entry : std::filesystem::directory_iterator(ryujinx_bis_user_save)) {
            if (!entry.is_directory()) {
                continue;
            }
            const auto title_word = read_le_u64(entry.path() / "ExtraData0", 0);
            if (!title_word.has_value()) {
                continue;
            }
            const auto name = title_id_from_u64(*title_word);
            if (looks_like_title_id(name)) {
                titles.insert(name);
            }
        }
    }

    const auto canon_root = profile.user_directory / "switch" / "saves";
    if (std::filesystem::is_directory(canon_root)) {
        for (const auto& entry : std::filesystem::directory_iterator(canon_root)) {
            if (!entry.is_directory()) {
                continue;
            }
            const auto name = normalize_switch_title_id(entry.path().filename().string());
            if (looks_like_title_id(name)) {
                titles.insert(name);
            }
        }
    }

    std::vector<std::string> synced;
    synced.reserve(titles.size());
    for (const auto& title : titles) {
        ensure_canonical_switch_save(profile, title);
        link_yuzu_save_to_canonical(profile, title);
        mirror_ryujinx_saves_with_canonical(ryujinx_bis_user_save, profile, title);
        synced.push_back(title);
    }
    std::sort(synced.begin(), synced.end());
    return synced;
}

std::vector<std::string> sync_switch_shared_saves_for_profile(const SaveProfile& profile) {
    const auto yuzu_nand =
        profile.user_directory / "yuzu" / "xdg-data" / "yuzu" / "nand" / "user" / "save" /
        "0000000000000000";
    const auto ryujinx_bis =
        profile.user_directory / "ryujinx" / "xdg-config" / "Ryujinx" / "bis" / "user" / "save";
    std::filesystem::create_directories(profile.user_directory / "switch" / "saves");
    return sync_switch_shared_saves(profile, yuzu_nand, ryujinx_bis);
}

} // namespace archstreamer
