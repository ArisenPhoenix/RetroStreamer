#include "host/switch_save_share.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <system_error>
#include <unordered_set>

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
    return value.size() == 16 && value.rfind("0100", 0) == 0;
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

bool replace_with_symlink(const std::filesystem::path& link_path, const std::filesystem::path& target) {
    std::error_code ec;
    if (std::filesystem::is_symlink(link_path, ec)) {
        const auto current = std::filesystem::read_symlink(link_path, ec);
        if (!ec && current == target) {
            return true;
        }
        std::filesystem::remove(link_path, ec);
    } else if (std::filesystem::exists(link_path, ec)) {
        if (std::filesystem::is_directory(link_path) && std::filesystem::is_empty(link_path, ec)) {
            std::filesystem::remove(link_path, ec);
        } else if (std::filesystem::is_directory(link_path)) {
            // Move any residual files into target first, then remove.
            for (const auto& entry : std::filesystem::directory_iterator(link_path, ec)) {
                if (ec) {
                    break;
                }
                if (!entry.is_regular_file()) {
                    continue;
                }
                const auto dest = target / entry.path().filename();
                if (!std::filesystem::exists(dest)) {
                    std::filesystem::rename(entry.path(), dest, ec);
                }
            }
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
        // Case-insensitive match for existing folders.
        for (const auto& title : std::filesystem::directory_iterator(user_dir.path())) {
            if (!title.is_directory() && !title.is_symlink()) {
                continue;
            }
            if (normalize_switch_title_id(title.path().filename().string()) == want) {
                return title.path();
            }
        }
    }
    // Default location under first/only user hash if present, else create placeholder hash dir.
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
        for (const auto& entry : std::filesystem::directory_iterator(yuzu_dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const auto dest = canon / entry.path().filename();
            if (!std::filesystem::exists(dest)) {
                std::error_code ec;
                std::filesystem::copy_file(entry.path(), dest, ec);
            }
        }
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

int link_ryujinx_saves_to_canonical(
    const std::filesystem::path& ryujinx_bis_user_save,
    const SaveProfile& profile,
    std::string_view title_id) {
    if (!std::filesystem::is_directory(ryujinx_bis_user_save)) {
        return 0;
    }
    const auto want = normalize_switch_title_id(title_id);
    const auto canon = ensure_canonical_switch_save(profile, title_id);
    int linked = 0;
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
        const auto leaf = entry.path() / "0";
        if (replace_with_symlink(leaf, canon)) {
            ++linked;
        }
    }
    return linked;
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

    // Also pick up titles already under canonical.
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
        link_ryujinx_saves_to_canonical(ryujinx_bis_user_save, profile, title);
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
