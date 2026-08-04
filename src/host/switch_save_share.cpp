#include "host/switch_save_share.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace archstreamer {
namespace {

constexpr const char* kClaimedFromTitleId = ".claimed_from_title_id";
constexpr const char* kClaimedByStem = ".claimed_by_stem";
constexpr const char* kTitleIdSidecar = ".title_id";

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

std::string to_lower_copy(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
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
            const auto name = entry.path().filename().string();
            if (!name.empty() && name.front() == '.') {
                continue; // skip sidecar markers
            }
            names.insert(name);
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

    std::filesystem::remove(leaf, ec);
    if (ec) {
        std::filesystem::remove_all(staging, ec);
        return false;
    }
    std::filesystem::rename(staging, leaf, ec);
    return !ec;
}

bool replace_with_symlink(
    const std::filesystem::path& link_path,
    const std::filesystem::path& target,
    bool absorb_existing_into_target) {
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
            if (absorb_existing_into_target) {
                mirror_files_newer_wins(link_path, target);
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
    return nand_save / "00000000000000000000000000000000" / want;
}

bool directory_has_save_payload(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (!name.empty() && name.front() == '.') {
            continue;
        }
        if (std::filesystem::file_size(entry.path(), ec) > 0 && !ec) {
            return true;
        }
    }
    return false;
}

void write_text_file(const std::filesystem::path& path, std::string_view text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (out) {
        out << text;
        if (!text.empty() && text.back() != '\n') {
            out << '\n';
        }
    }
}

std::string read_text_file_trimmed(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    auto value = ss.str();
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ')) {
        value.pop_back();
    }
    return value;
}

void write_title_id_sidecar(const std::filesystem::path& stem_dir, std::string_view title_id) {
    if (title_id.empty() || !looks_like_title_id(title_id)) {
        return;
    }
    write_text_file(stem_dir / kTitleIdSidecar, normalize_switch_title_id(title_id));
}

std::string read_title_id_sidecar(const std::filesystem::path& stem_dir) {
    const auto value = read_text_file_trimmed(stem_dir / kTitleIdSidecar);
    if (looks_like_title_id(value)) {
        return normalize_switch_title_id(value);
    }
    const auto claimed = read_text_file_trimmed(stem_dir / kClaimedFromTitleId);
    if (looks_like_title_id(claimed)) {
        return normalize_switch_title_id(claimed);
    }
    return {};
}

bool claim_legacy_title_id_once(
    const SaveProfile& profile,
    std::string_view content_stem,
    std::string_view title_id,
    const std::filesystem::path& stem_dir) {
    if (title_id.empty() || !looks_like_title_id(title_id)) {
        return false;
    }
    if (directory_has_save_payload(stem_dir)) {
        return false;
    }
    const auto legacy = legacy_title_id_switch_save_directory(profile, title_id);
    if (!directory_has_save_payload(legacy)) {
        return false;
    }
    const auto claimed_by = read_text_file_trimmed(legacy / kClaimedByStem);
    if (!claimed_by.empty() && claimed_by != content_stem) {
        std::cout
            << "switch save share: legacy " << normalize_switch_title_id(title_id)
            << " already claimed by \"" << claimed_by << "\"; "
            << content_stem << " starts empty\n";
        return false;
    }

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(legacy, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (!name.empty() && name.front() == '.') {
            continue;
        }
        copy_file_overwrite_preserve_mtime(entry.path(), stem_dir / name);
    }
    write_text_file(stem_dir / kClaimedFromTitleId, normalize_switch_title_id(title_id));
    write_text_file(legacy / kClaimedByStem, content_stem);
    write_title_id_sidecar(stem_dir, title_id);
    std::cout
        << "switch save share: claimed legacy " << normalize_switch_title_id(title_id)
        << " → \"" << content_stem << "\"\n";
    return true;
}

void clear_save_payload_files(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (!name.empty() && name.front() == '.') {
            continue;
        }
        std::filesystem::remove(entry.path(), ec);
    }
}

void replace_directory_payload_with_stem(
    const std::filesystem::path& bank,
    const std::filesystem::path& stem) {
    std::error_code ec;
    std::filesystem::create_directories(bank, ec);
    clear_save_payload_files(bank);
    if (!std::filesystem::is_directory(stem, ec)) {
        return;
    }
    for (const auto& file : std::filesystem::directory_iterator(stem, ec)) {
        if (ec) {
            break;
        }
        if (!file.is_regular_file()) {
            continue;
        }
        const auto name = file.path().filename().string();
        if (!name.empty() && name.front() == '.') {
            continue;
        }
        copy_file_overwrite_preserve_mtime(file.path(), bank / name);
    }
}

using BisAccountFn = bool (*)(
    const std::filesystem::path& account_dir,
    const std::filesystem::path& slot0,
    const std::filesystem::path& slot1,
    const std::filesystem::path& canon);

int for_each_ryujinx_title_account_save(
    const std::filesystem::path& ryujinx_bis_user_save,
    std::string_view title_id,
    const std::filesystem::path& canon,
    BisAccountFn fn) {
    if (!std::filesystem::is_directory(ryujinx_bis_user_save) || title_id.empty() || !fn) {
        return 0;
    }
    const auto want = normalize_switch_title_id(title_id);
    int touched = 0;
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
        if (fn(entry.path(), slot0, slot1, canon)) {
            ++touched;
        }
    }
    return touched;
}

int mirror_ryujinx_to_canon_path(
    const std::filesystem::path& ryujinx_bis_user_save,
    const std::filesystem::path& canon,
    std::string_view title_id) {
    return for_each_ryujinx_title_account_save(
        ryujinx_bis_user_save,
        title_id,
        canon,
        [](const std::filesystem::path&,
           const std::filesystem::path& slot0,
           const std::filesystem::path& slot1,
           const std::filesystem::path& stem) {
            mirror_files_newer_wins(slot0, slot1);
            mirror_files_newer_wins(slot0, stem);
            mirror_files_newer_wins(slot0, slot1);
            return true;
        });
}

/** Launch path: stem is authoritative — wipe title banks and copy stem in (empty stem → empty BIS). */
int replace_ryujinx_bis_with_canon(
    const std::filesystem::path& ryujinx_bis_user_save,
    const std::filesystem::path& canon,
    std::string_view title_id) {
    return for_each_ryujinx_title_account_save(
        ryujinx_bis_user_save,
        title_id,
        canon,
        [](const std::filesystem::path&,
           const std::filesystem::path& slot0,
           const std::filesystem::path& slot1,
           const std::filesystem::path& stem) {
            replace_directory_payload_with_stem(slot0, stem);
            replace_directory_payload_with_stem(slot1, stem);
            return true;
        });
}

bool link_yuzu_to_canon(
    const SaveProfile& profile,
    std::string_view title_id,
    const std::filesystem::path& canon,
    bool absorb_existing_into_target) {
    if (title_id.empty()) {
        return false;
    }
    auto yuzu_dir = yuzu_title_save_directory(profile, title_id);
    if (yuzu_dir.empty()) {
        return false;
    }
    std::filesystem::create_directories(yuzu_dir.parent_path());
    return replace_with_symlink(yuzu_dir, canon, absorb_existing_into_target);
}

std::string title_id_from_update_nsps_for_stem(std::string_view content_stem) {
    // Match UPD NSP names like: Pokemon Shield [01008DB008C2C800][v1.3.2][UPD].nsp
    // Application id ≈ patch id with bit 0x800 cleared.
    const auto stem_l = to_lower_copy(content_stem);
    // Strip trailing version tokens for matching ("pokemon shield 1.3.2" → "pokemon shield").
    std::string base_name = stem_l;
    static const std::regex version_suffix(R"((?:\s+|[_-])v?\d+(?:\.\d+){1,3}\s*$)", std::regex::icase);
    base_name = std::regex_replace(base_name, version_suffix, "");
    while (!base_name.empty() && base_name.back() == ' ') {
        base_name.pop_back();
    }
    if (base_name.size() < 3) {
        base_name = stem_l;
    }

    const auto updates = std::filesystem::path{"/mnt/Internal_SSD/Gaming/ROMS/SwitchUpdates"};
    // Also try env via a lightweight reimplementation to avoid circular deps — caller may pass later.
    std::vector<std::filesystem::path> roots{updates};
    if (const char* env = std::getenv("ARCHSTREAMER_SWITCH_UPDATES");
        env != nullptr && *env != '\0') {
        roots.insert(roots.begin(), std::filesystem::path(env));
    }

    static const std::regex tid_re(R"(\[([0-9A-Fa-f]{16})\])");
    for (const auto& root : roots) {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) {
            continue;
        }
        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const auto name = entry.path().filename().string();
            const auto name_l = to_lower_copy(name);
            if (name_l.find("[upd]") == std::string::npos &&
                name_l.find("][upd]") == std::string::npos) {
                // Accept either [UPD] tag.
                if (name_l.find("upd]") == std::string::npos) {
                    continue;
                }
            }
            if (name_l.find(base_name) == std::string::npos) {
                continue;
            }
            std::smatch match;
            if (!std::regex_search(name, match, tid_re) || match.size() < 2) {
                continue;
            }
            auto patch = normalize_switch_title_id(match[1].str());
            if (!looks_like_title_id(patch)) {
                continue;
            }
            // Patch title ids typically set 0x800 in the low nibble of the last byte group.
            std::uint64_t value = 0;
            for (char ch : patch) {
                value <<= 4;
                if (ch >= '0' && ch <= '9') {
                    value |= static_cast<std::uint64_t>(ch - '0');
                } else {
                    value |= static_cast<std::uint64_t>(10 + ch - 'a');
                }
            }
            value &= ~0x800ull;
            return title_id_from_u64(value);
        }
    }
    return {};
}

std::string title_id_from_bis(const SaveProfile& profile, std::string_view preferred) {
    const auto ryujinx_bis =
        profile.user_directory / "ryujinx" / "xdg-config" / "Ryujinx" / "bis" / "user" / "save";
    std::error_code ec;
    if (!std::filesystem::is_directory(ryujinx_bis, ec)) {
        return {};
    }
    std::string fallback;
    for (const auto& entry : std::filesystem::directory_iterator(ryujinx_bis, ec)) {
        if (!entry.is_directory()) {
            continue;
        }
        const auto title_word = read_le_u64(entry.path() / "ExtraData0", 0);
        if (!title_word.has_value()) {
            continue;
        }
        const auto kind = read_le_u64(entry.path() / "ExtraData0", 8);
        if (!kind.has_value() || (*kind & 0xffull) != 1ull) {
            continue;
        }
        const auto tid = title_id_from_u64(*title_word);
        if (!preferred.empty() && tid == normalize_switch_title_id(preferred)) {
            return tid;
        }
        if (fallback.empty()) {
            fallback = tid;
        }
    }
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

bool looks_like_switch_title_id(std::string_view value) {
    return looks_like_title_id(normalize_switch_title_id(value));
}

std::filesystem::path canonical_catalog_switch_save_directory(
    const SaveProfile& profile,
    std::string_view content_stem) {
    return profile.user_directory / "switch" / "saves" / std::string(content_stem);
}

std::filesystem::path legacy_title_id_switch_save_directory(
    const SaveProfile& profile,
    std::string_view title_id) {
    return profile.user_directory / "switch" / "saves" / normalize_switch_title_id(title_id);
}

std::filesystem::path catalog_switch_addon_directory(
    const SaveProfile& profile,
    std::string_view content_stem) {
    return profile.user_directory / "switch" / "addons" / std::string(content_stem);
}

std::filesystem::path ensure_catalog_switch_save(
    const SaveProfile& profile,
    std::string_view content_stem,
    std::string_view title_id) {
    if (content_stem.empty()) {
        return {};
    }
    const auto stem_dir = canonical_catalog_switch_save_directory(profile, content_stem);
    std::error_code ec;
    // Legacy layout used friendly-name symlinks → title-id. Replace with a real stem dir.
    if (std::filesystem::is_symlink(stem_dir, ec)) {
        std::filesystem::remove(stem_dir, ec);
    }
    std::filesystem::create_directories(stem_dir);

    std::string tid = normalize_switch_title_id(title_id);
    if (tid.empty()) {
        tid = read_title_id_sidecar(stem_dir);
    }
    if (!tid.empty()) {
        (void)claim_legacy_title_id_once(profile, content_stem, tid, stem_dir);
        write_title_id_sidecar(stem_dir, tid);
    }
    return stem_dir;
}

std::string resolve_switch_title_id_for_catalog(
    const SaveProfile& profile,
    std::string_view content_stem,
    const std::filesystem::path& content_path) {
    (void)content_path;
    if (content_stem.empty()) {
        return {};
    }
    const auto stem_dir = canonical_catalog_switch_save_directory(profile, content_stem);
    if (auto tid = read_title_id_sidecar(stem_dir); !tid.empty()) {
        return tid;
    }
    if (auto tid = title_id_from_update_nsps_for_stem(content_stem); !tid.empty()) {
        write_title_id_sidecar(stem_dir, tid);
        return tid;
    }
    // If legacy title-id folder exists and is unclaimed or claimed by this stem, use it.
    const auto saves_root = profile.user_directory / "switch" / "saves";
    std::error_code ec;
    if (std::filesystem::is_directory(saves_root, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(saves_root, ec)) {
            if (!entry.is_directory()) {
                continue;
            }
            const auto leaf = entry.path().filename().string();
            if (!looks_like_title_id(leaf)) {
                continue;
            }
            const auto claimed_by = read_text_file_trimmed(entry.path() / kClaimedByStem);
            if (!claimed_by.empty() && claimed_by != content_stem) {
                continue;
            }
            if (directory_has_save_payload(entry.path()) || claimed_by == content_stem) {
                const auto tid = normalize_switch_title_id(leaf);
                write_title_id_sidecar(stem_dir, tid);
                return tid;
            }
        }
    }
    return title_id_from_bis(profile, {});
}

std::string sync_catalog_switch_save_for_launch(
    const SaveProfile& profile,
    std::string_view content_stem,
    std::string_view title_id) {
    if (content_stem.empty()) {
        return {};
    }
    auto tid = normalize_switch_title_id(title_id);
    if (tid.empty()) {
        tid = resolve_switch_title_id_for_catalog(profile, content_stem);
    }
    const auto canon = ensure_catalog_switch_save(profile, content_stem, tid);
    if (canon.empty()) {
        return {};
    }
    if (!tid.empty()) {
        // Stem is source of truth: never absorb leftover yuzu/BIS bytes into it.
        link_yuzu_to_canon(profile, tid, canon, /*absorb_existing_into_target=*/false);
        const auto ryujinx_bis =
            profile.user_directory / "ryujinx" / "xdg-config" / "Ryujinx" / "bis" / "user" / "save";
        const int replaced = replace_ryujinx_bis_with_canon(ryujinx_bis, canon, tid);
        std::cout
            << "switch save share: launch \"" << content_stem << "\" → BIS title "
            << tid << " (" << replaced << " account save(s) replaced)\n";
    }
    return std::string(content_stem);
}

std::string sync_catalog_switch_save_after_exit(
    const SaveProfile& profile,
    std::string_view content_stem,
    std::string_view title_id) {
    if (content_stem.empty()) {
        return {};
    }
    auto tid = normalize_switch_title_id(title_id);
    if (tid.empty()) {
        tid = resolve_switch_title_id_for_catalog(profile, content_stem);
    }
    const auto canon = ensure_catalog_switch_save(profile, content_stem, tid);
    if (canon.empty()) {
        return {};
    }
    if (!tid.empty()) {
        const auto ryujinx_bis =
            profile.user_directory / "ryujinx" / "xdg-config" / "Ryujinx" / "bis" / "user" / "save";
        // Session just ran against this stem's BIS image — pull progress back into the stem only.
        (void)mirror_ryujinx_to_canon_path(ryujinx_bis, canon, tid);
        write_title_id_sidecar(canon, tid);
        link_yuzu_to_canon(profile, tid, canon, /*absorb_existing_into_target=*/false);
    }
    return std::string(content_stem);
}

std::filesystem::path canonical_switch_save_directory(
    const SaveProfile& profile,
    std::string_view title_id) {
    return legacy_title_id_switch_save_directory(profile, title_id);
}

std::filesystem::path ensure_canonical_switch_save(
    const SaveProfile& profile,
    std::string_view title_id) {
    const auto canon = legacy_title_id_switch_save_directory(profile, title_id);
    std::filesystem::create_directories(canon);
    return canon;
}

bool link_yuzu_save_to_canonical(const SaveProfile& profile, std::string_view title_id) {
    const auto canon = ensure_canonical_switch_save(profile, title_id);
    return link_yuzu_to_canon(profile, title_id, canon, /*absorb_existing_into_target=*/true);
}

int mirror_ryujinx_saves_with_canonical(
    const std::filesystem::path& ryujinx_bis_user_save,
    const SaveProfile& profile,
    std::string_view title_id) {
    const auto canon = ensure_canonical_switch_save(profile, title_id);
    return mirror_ryujinx_to_canon_path(ryujinx_bis_user_save, canon, title_id);
}

std::vector<std::string> sync_switch_shared_saves(
    const SaveProfile& profile,
    const std::filesystem::path& yuzu_nand_user_save,
    const std::filesystem::path& ryujinx_bis_user_save) {
    (void)yuzu_nand_user_save;
    (void)ryujinx_bis_user_save;
    // Discovery-only: do not remirror every title into one leaf.
    std::unordered_set<std::string> leaves;
    const auto canon_root = profile.user_directory / "switch" / "saves";
    std::error_code ec;
    if (std::filesystem::is_directory(canon_root, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(canon_root, ec)) {
            if (entry.is_directory() || entry.is_symlink()) {
                leaves.insert(entry.path().filename().string());
            }
        }
    }
    std::vector<std::string> synced(leaves.begin(), leaves.end());
    std::sort(synced.begin(), synced.end());
    return synced;
}

std::vector<std::string> sync_switch_shared_saves_for_profile(const SaveProfile& profile) {
    std::filesystem::create_directories(profile.user_directory / "switch" / "saves");
    std::filesystem::create_directories(profile.user_directory / "switch" / "addons");
    const auto yuzu_nand =
        profile.user_directory / "yuzu" / "xdg-data" / "yuzu" / "nand" / "user" / "save" /
        "0000000000000000";
    const auto ryujinx_bis =
        profile.user_directory / "ryujinx" / "xdg-config" / "Ryujinx" / "bis" / "user" / "save";
    return sync_switch_shared_saves(profile, yuzu_nand, ryujinx_bis);
}

} // namespace archstreamer
