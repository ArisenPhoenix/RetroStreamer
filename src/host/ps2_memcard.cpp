#include "host/ps2_memcard.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <iconv.h>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace archstreamer {
namespace {

constexpr std::string_view kMagic = "Sony PS2 Memory Card Format ";
constexpr std::uint16_t kDfExists = 0x8000;
constexpr std::uint16_t kDfDirectory = 0x0020;
constexpr std::uint16_t kDfFile = 0x0010;

std::uint16_t read_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint32_t read_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0])
        | (static_cast<std::uint32_t>(p[1]) << 8)
        | (static_cast<std::uint32_t>(p[2]) << 16)
        | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::string ascii_upper(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string compact_alnum(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        const auto u = static_cast<unsigned char>(ch);
        if (std::isalnum(u)) {
            out.push_back(static_cast<char>(std::tolower(u)));
        }
    }
    return out;
}

/** Decode UTF-8 and fold fullwidth Latin / digits to ASCII; keep other ASCII. */
std::string fold_to_ascii_alnum(std::string_view utf8) {
    std::string out;
    out.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size();) {
        const auto c0 = static_cast<unsigned char>(utf8[i]);
        if (c0 < 0x80) {
            if (std::isalnum(c0)) {
                out.push_back(static_cast<char>(std::tolower(c0)));
            }
            ++i;
            continue;
        }
        // Fullwidth forms U+FF01–U+FF5E are UTF-8 EF BC/BD …
        if (c0 == 0xEF && i + 2 < utf8.size()) {
            const auto c1 = static_cast<unsigned char>(utf8[i + 1]);
            const auto c2 = static_cast<unsigned char>(utf8[i + 2]);
            char32_t cp = 0;
            if (c1 == 0xBC) {
                cp = 0xFF00u + (c2 - 0x80u);
            } else if (c1 == 0xBD) {
                cp = 0xFF40u + (c2 - 0x80u);
            }
            if (cp >= 0xFF01 && cp <= 0xFF5E) {
                const auto ascii = static_cast<unsigned char>(cp - 0xFEE0);
                if (std::isalnum(ascii)) {
                    out.push_back(static_cast<char>(std::tolower(ascii)));
                }
                i += 3;
                continue;
            }
        }
        // Skip other multibyte sequences (ideographic space, stars, etc.).
        if ((c0 & 0xF8) == 0xF0) {
            i += 4;
        } else if ((c0 & 0xF0) == 0xE0) {
            i += 3;
        } else if ((c0 & 0xE0) == 0xC0) {
            i += 2;
        } else {
            ++i;
        }
    }
    return out;
}

std::string sjis_to_utf8(std::string_view sjis) {
    iconv_t cd = iconv_open("UTF-8", "SHIFT_JIS");
    if (cd == reinterpret_cast<iconv_t>(-1)) {
        return {};
    }
    std::string in(sjis);
    std::string out(sjis.size() * 4 + 16, '\0');
    char* inbuf = in.data();
    std::size_t inleft = in.size();
    char* outbuf = out.data();
    std::size_t outleft = out.size();
    const auto n = iconv(cd, &inbuf, &inleft, &outbuf, &outleft);
    iconv_close(cd);
    if (n == static_cast<std::size_t>(-1)) {
        return {};
    }
    out.resize(out.size() - outleft);
    return out;
}

std::string title_from_icon_sys(const std::vector<std::uint8_t>& file) {
    if (file.size() < 0xC0 + 2) {
        return {};
    }
    if (file[0] != 'P' || file[1] != 'S' || file[2] != '2' || file[3] != 'D') {
        return {};
    }
    constexpr std::size_t kTitleOff = 0xC0;
    constexpr std::size_t kTitleLen = 68;
    const auto avail = std::min(kTitleLen, file.size() - kTitleOff);
    std::string_view raw(
        reinterpret_cast<const char*>(file.data() + kTitleOff),
        avail);
    const auto nul = raw.find('\0');
    if (nul != std::string_view::npos) {
        raw = raw.substr(0, nul);
    }
    return fold_to_ascii_alnum(sjis_to_utf8(raw));
}

std::optional<std::string> product_code_from_save_name(std::string_view name) {
    if (name.size() < 8) {
        return std::nullopt;
    }
    const char b = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    if (b != 'B') {
        return std::nullopt;
    }
    // B + region + PRODUCT (e.g. BASLUS-20370-01 → SLUS-20370).
    std::string rest(name.substr(2));
    rest = ascii_upper(std::move(rest));
    if (rest.size() < 9) {
        return std::nullopt;
    }
    for (int i = 0; i < 4; ++i) {
        if (!std::isalpha(static_cast<unsigned char>(rest[static_cast<std::size_t>(i)]))) {
            return std::nullopt;
        }
    }
    std::size_t digits_at = 4;
    if (rest[4] == '-') {
        digits_at = 5;
    }
    if (digits_at + 5 > rest.size()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < 5; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(rest[digits_at + i]))) {
            return std::nullopt;
        }
    }
    return rest.substr(0, 4) + "-" + rest.substr(digits_at, 5);
}

struct Superblock {
    std::uint16_t page_len = 0;
    std::uint16_t pages_per_cluster = 0;
    std::uint32_t clusters_per_card = 0;
    std::uint32_t alloc_offset = 0;
    std::uint32_t rootdir_cluster = 0;
    std::uint32_t ifc_list[32]{};
    std::uint32_t page_stride = 0;
    std::uint32_t cluster_size = 0;
};

struct DirEntry {
    std::uint16_t mode = 0;
    std::uint32_t length = 0;
    std::uint32_t cluster = 0;
    std::string name;
};

bool parse_superblock(const std::vector<std::uint8_t>& data, Superblock& sb) {
    if (data.size() < 0x154) {
        return false;
    }
    if (std::memcmp(data.data(), kMagic.data(), kMagic.size()) != 0) {
        return false;
    }
    sb.page_len = read_u16(data.data() + 0x28);
    sb.pages_per_cluster = read_u16(data.data() + 0x2A);
    sb.clusters_per_card = read_u32(data.data() + 0x30);
    sb.alloc_offset = read_u32(data.data() + 0x34);
    sb.rootdir_cluster = read_u32(data.data() + 0x3C);
    for (int i = 0; i < 32; ++i) {
        sb.ifc_list[i] = read_u32(data.data() + 0x50 + static_cast<std::size_t>(i) * 4);
    }
    if (sb.page_len == 0 || sb.pages_per_cluster == 0 || sb.clusters_per_card == 0) {
        return false;
    }
    sb.cluster_size = static_cast<std::uint32_t>(sb.page_len) * sb.pages_per_cluster;
    const auto spare = (sb.page_len / 128u) * 4u;
    const auto pages = static_cast<std::uint64_t>(sb.clusters_per_card) * sb.pages_per_cluster;
    const auto raw_no_ecc = pages * sb.page_len;
    const auto raw_ecc = pages * (sb.page_len + spare);
    if (data.size() >= raw_ecc) {
        sb.page_stride = sb.page_len + spare;
    } else if (data.size() >= raw_no_ecc) {
        sb.page_stride = sb.page_len;
    } else {
        sb.page_stride = sb.page_len;
    }
    return true;
}

bool read_cluster(
    const std::vector<std::uint8_t>& data,
    const Superblock& sb,
    std::uint32_t absolute_cluster,
    std::vector<std::uint8_t>& out) {
    const auto page0 = static_cast<std::uint64_t>(absolute_cluster) * sb.pages_per_cluster;
    out.assign(sb.cluster_size, 0);
    for (std::uint16_t p = 0; p < sb.pages_per_cluster; ++p) {
        const auto off = (page0 + p) * sb.page_stride;
        if (off + sb.page_len > data.size()) {
            return false;
        }
        std::memcpy(
            out.data() + static_cast<std::size_t>(p) * sb.page_len,
            data.data() + static_cast<std::size_t>(off),
            sb.page_len);
    }
    return true;
}

std::optional<std::uint32_t> fat_entry(
    const std::vector<std::uint8_t>& data,
    const Superblock& sb,
    std::uint32_t relative_cluster) {
    const std::uint32_t entries_per_cluster = sb.cluster_size / 4;
    if (entries_per_cluster == 0) {
        return std::nullopt;
    }
    const auto fat_offset = relative_cluster % entries_per_cluster;
    const auto indirect_index = relative_cluster / entries_per_cluster;
    const auto indirect_offset = indirect_index % entries_per_cluster;
    const auto dbl_indirect_index = indirect_index / entries_per_cluster;
    if (dbl_indirect_index >= 32) {
        return std::nullopt;
    }
    const auto indirect_cluster_num = sb.ifc_list[dbl_indirect_index];
    if (indirect_cluster_num == 0xFFFFFFFFu) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> indirect;
    if (!read_cluster(data, sb, indirect_cluster_num, indirect)) {
        return std::nullopt;
    }
    const auto fat_cluster_num = read_u32(indirect.data() + indirect_offset * 4);
    if (fat_cluster_num == 0xFFFFFFFFu) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> fat;
    if (!read_cluster(data, sb, fat_cluster_num, fat)) {
        return std::nullopt;
    }
    return read_u32(fat.data() + fat_offset * 4);
}

bool walk_cluster_chain(
    const std::vector<std::uint8_t>& data,
    const Superblock& sb,
    std::uint32_t first_relative,
    const std::function<bool(const std::vector<std::uint8_t>&)>& on_cluster) {
    std::uint32_t rel = first_relative;
    for (int guard = 0; guard < 100000; ++guard) {
        if (rel == 0xFFFFFFFFu) {
            return true;
        }
        std::vector<std::uint8_t> cluster;
        if (!read_cluster(data, sb, sb.alloc_offset + rel, cluster)) {
            return false;
        }
        if (!on_cluster(cluster)) {
            return true;
        }
        const auto next = fat_entry(data, sb, rel);
        if (!next) {
            return false;
        }
        if ((*next & 0x80000000u) == 0) {
            return false;
        }
        const auto nxt = *next & 0x7FFFFFFFu;
        if (nxt == 0x7FFFFFFFu) {
            return true;
        }
        rel = nxt;
    }
    return false;
}

DirEntry parse_dir_entry(const std::uint8_t* p) {
    DirEntry e;
    e.mode = read_u16(p + 0x00);
    e.length = read_u32(p + 0x04);
    e.cluster = read_u32(p + 0x10);
    char name[33]{};
    std::memcpy(name, p + 0x40, 32);
    e.name = name;
    const auto nul = e.name.find('\0');
    if (nul != std::string::npos) {
        e.name.resize(nul);
    }
    return e;
}

bool read_directory(
    const std::vector<std::uint8_t>& data,
    const Superblock& sb,
    std::uint32_t first_relative,
    std::uint32_t entry_count,
    std::vector<DirEntry>& out) {
    out.clear();
    const auto entries_per_cluster = sb.cluster_size / 512;
    if (entries_per_cluster == 0) {
        return false;
    }
    std::uint32_t seen = 0;
    const bool ok = walk_cluster_chain(data, sb, first_relative, [&](const std::vector<std::uint8_t>& cluster) {
        for (std::uint32_t i = 0; i < entries_per_cluster; ++i) {
            if (entry_count != 0 && seen >= entry_count) {
                return false;
            }
            out.push_back(parse_dir_entry(cluster.data() + i * 512));
            ++seen;
        }
        return entry_count == 0 || seen < entry_count;
    });
    if (!ok && out.empty()) {
        return false;
    }
    if (entry_count != 0 && out.size() > entry_count) {
        out.resize(entry_count);
    }
    return true;
}

bool read_file_bytes(
    const std::vector<std::uint8_t>& data,
    const Superblock& sb,
    std::uint32_t first_relative,
    std::uint32_t length,
    std::vector<std::uint8_t>& out) {
    out.clear();
    out.reserve(length);
    const bool ok = walk_cluster_chain(data, sb, first_relative, [&](const std::vector<std::uint8_t>& cluster) {
        if (out.size() >= length) {
            return false;
        }
        const auto need = length - static_cast<std::uint32_t>(out.size());
        const auto take = std::min<std::uint32_t>(need, static_cast<std::uint32_t>(cluster.size()));
        out.insert(out.end(), cluster.begin(), cluster.begin() + take);
        return out.size() < length;
    });
    if (!ok && out.empty()) {
        return false;
    }
    if (out.size() > length) {
        out.resize(length);
    }
    return true;
}

struct SaveFolderStats {
    std::uint64_t size_bytes = 0;
    std::string title;
};

SaveFolderStats directory_stats(
    const std::vector<std::uint8_t>& data,
    const Superblock& sb,
    std::uint32_t first_relative,
    std::uint32_t entry_count,
    int depth = 0) {
    SaveFolderStats stats;
    if (depth > 8) {
        return stats;
    }
    std::vector<DirEntry> entries;
    if (!read_directory(data, sb, first_relative, entry_count, entries)) {
        return stats;
    }
    for (const auto& e : entries) {
        if ((e.mode & kDfExists) == 0) {
            continue;
        }
        if (e.name == "." || e.name == "..") {
            continue;
        }
        if (e.mode & kDfFile) {
            stats.size_bytes += e.length;
            if (stats.title.empty()) {
                std::string lower = e.name;
                for (char& ch : lower) {
                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                }
                if (lower == "icon.sys") {
                    std::vector<std::uint8_t> file;
                    if (read_file_bytes(data, sb, e.cluster, e.length, file)) {
                        stats.title = title_from_icon_sys(file);
                    }
                }
            }
        } else if (e.mode & kDfDirectory) {
            const auto nested = directory_stats(data, sb, e.cluster, e.length, depth + 1);
            stats.size_bytes += nested.size_bytes;
            if (stats.title.empty() && !nested.title.empty()) {
                stats.title = nested.title;
            }
        }
    }
    return stats;
}

/** True when title_c is the game (or game + non-letter suffix like slot digits). */
bool title_matches_game(std::string_view title_c, std::string_view game_c) {
    if (game_c.size() < 4 || title_c.size() < 4) {
        return false;
    }
    if (title_c == game_c) {
        return true;
    }
    if (!title_c.starts_with(game_c)) {
        return false;
    }
    // Reject Kingdom Hearts matching a Kingdom Hearts II title prefix.
    const auto next = static_cast<unsigned char>(title_c[game_c.size()]);
    return !std::isalpha(next);
}

std::mutex g_ps2_cache_mu;
bool g_ps2_scan_done = false;
bool g_ps2_scan_running = false;
std::unordered_map<std::string, std::optional<Ps2MemcardUsage>> g_ps2_cache;

std::string cache_key(const std::filesystem::path& path) {
    std::error_code ec;
    const auto canon = std::filesystem::weakly_canonical(path, ec);
    return (ec ? path : canon).string();
}

std::optional<Ps2MemcardUsage> parse_ps2_memcard_usage(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> data(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());
    Superblock sb;
    if (!parse_superblock(data, sb)) {
        return std::nullopt;
    }

    std::vector<DirEntry> root_probe;
    if (!read_directory(data, sb, sb.rootdir_cluster, 2, root_probe) || root_probe.empty()) {
        return std::nullopt;
    }
    const auto root_count = root_probe[0].length;
    std::vector<DirEntry> root;
    if (!read_directory(data, sb, sb.rootdir_cluster, root_count, root)) {
        return std::nullopt;
    }

    Ps2MemcardUsage usage;
    usage.capacity_bytes =
        static_cast<std::uint64_t>(sb.clusters_per_card) * sb.cluster_size;
    for (const auto& e : root) {
        if ((e.mode & kDfExists) == 0) {
            continue;
        }
        if (!(e.mode & kDfDirectory)) {
            continue;
        }
        if (e.name.empty() || e.name == "." || e.name == "..") {
            continue;
        }
        bool printable = true;
        for (unsigned char ch : e.name) {
            if (ch < 32 || ch > 126) {
                printable = false;
                break;
            }
        }
        if (!printable) {
            continue;
        }
        const auto folder = directory_stats(data, sb, e.cluster, e.length);
        Ps2MemcardSaveEntry save;
        save.name = e.name;
        if (auto code = product_code_from_save_name(e.name)) {
            save.product_code = std::move(*code);
        }
        save.title = folder.title;
        save.size_bytes = folder.size_bytes;
        usage.used_bytes += save.size_bytes;
        usage.saves.push_back(std::move(save));
    }
    return usage;
}

} // namespace

std::optional<Ps2MemcardUsage> read_ps2_memcard_usage(const std::filesystem::path& path) {
    const auto key = cache_key(path);
    std::lock_guard lock(g_ps2_cache_mu);
    const auto it = g_ps2_cache.find(key);
    return it == g_ps2_cache.end() ? std::nullopt : it->second;
}

void ps2_memcard_prewarm(const std::vector<std::filesystem::path>& paths) {
    {
        std::lock_guard lock(g_ps2_cache_mu);
        if (g_ps2_scan_done || g_ps2_scan_running) {
            return;
        }
        g_ps2_scan_running = true;
    }
    // Parse outside the lock so cache readers never wait on a multi-MiB image.
    std::vector<std::pair<std::string, std::optional<Ps2MemcardUsage>>> parsed;
    parsed.reserve(paths.size());
    for (const auto& path : paths) {
        parsed.emplace_back(cache_key(path), parse_ps2_memcard_usage(path));
    }

    std::lock_guard lock(g_ps2_cache_mu);
    for (auto& [key, usage] : parsed) {
        g_ps2_cache.insert_or_assign(key, std::move(usage));
    }
    g_ps2_scan_running = false;
    g_ps2_scan_done = true;
}

bool ps2_memcard_scan_complete() {
    std::lock_guard lock(g_ps2_cache_mu);
    return g_ps2_scan_done;
}

std::optional<std::uint64_t> ps2_memcard_bytes_for_game(
    const Ps2MemcardUsage& card,
    std::string_view display_name,
    std::string_view content_stem,
    std::string_view serial_or_product) {
    const auto serial_c = compact_alnum(serial_or_product);
    const auto display_c = compact_alnum(display_name);
    const auto stem_c = compact_alnum(content_stem);

    auto score_of = [&](const Ps2MemcardSaveEntry& save) -> std::size_t {
        const auto name_c = compact_alnum(save.name);
        const auto product_c = compact_alnum(save.product_code);
        const auto title_c = save.title; // already folded alnum

        if (!serial_c.empty()) {
            if ((!product_c.empty() && product_c == serial_c)
                || (!name_c.empty() && name_c.find(serial_c) != std::string::npos)) {
                return 300 + serial_c.size();
            }
        }
        if (!title_c.empty()) {
            if (title_matches_game(title_c, display_c)) {
                return 250 + display_c.size();
            }
            if (title_matches_game(title_c, stem_c)) {
                return 240 + stem_c.size();
            }
        }
        if (!stem_c.empty() && stem_c.size() >= 4
            && (name_c.find(stem_c) != std::string::npos
                || stem_c.find(name_c) != std::string::npos)) {
            return 200 + std::min(stem_c.size(), name_c.size());
        }
        if (!display_c.empty() && display_c.size() >= 4
            && (name_c.find(display_c) != std::string::npos
                || display_c.find(name_c) != std::string::npos)) {
            return 100 + std::min(display_c.size(), name_c.size());
        }
        return 0;
    };

    std::size_t best_tier = 0;
    for (const auto& save : card.saves) {
        const auto score = score_of(save);
        const std::size_t tier = score >= 300 ? 3 : score >= 240 ? 2 : score >= 100 ? 1 : 0;
        best_tier = std::max(best_tier, tier);
    }
    if (best_tier == 0) {
        return std::nullopt;
    }

    std::uint64_t total = 0;
    bool any = false;
    for (const auto& save : card.saves) {
        const auto score = score_of(save);
        const std::size_t tier = score >= 300 ? 3 : score >= 240 ? 2 : score >= 100 ? 1 : 0;
        if (tier == best_tier) {
            total += save.size_bytes;
            any = true;
        }
    }
    if (!any) {
        return std::nullopt;
    }
    return total;
}

} // namespace archstreamer
