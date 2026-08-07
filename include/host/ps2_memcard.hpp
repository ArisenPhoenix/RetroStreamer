#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace archstreamer {

/** One root-level save folder on a PS2 memory card (game save container). */
struct Ps2MemcardSaveEntry {
    std::string name; // directory name on the card (often BESLES-… / BASLUS-…)
    /** Product code when parseable from `name` (e.g. SLUS-20370). */
    std::string product_code;
    /**
     * ASCII-folded title from icon.sys when present (fullwidth Latin collapsed).
     * Used to match catalog display names when serial aliases are missing.
     */
    std::string title;
    std::uint64_t size_bytes = 0;
};

/**
 * Read-only summary of a PCSX2/raw `.ps2` memcard image.
 * capacity_bytes is the card data area (typically 8 MiB); used_bytes sums file
 * payloads under root saves (not FAT overhead).
 */
struct Ps2MemcardUsage {
    std::uint64_t capacity_bytes = 0;
    std::uint64_t used_bytes = 0;
    std::vector<Ps2MemcardSaveEntry> saves;
};

/**
 * Parse a PS2 memory-card image for per-save and total capacity.
 * Soft-fail: returns nullopt if the file is missing, unreadable, or not a
 * Sony PS2 Memory Card Format image.
 *
 * Disk parse runs only when `!scan_done && users_tab_opened`. After
 * `ps2_memcard_finish_initial_scan()`, later calls use the cache only.
 */
std::optional<Ps2MemcardUsage> read_ps2_memcard_usage(const std::filesystem::path& path);

/** Allow the first memcard parse (call when the Users tab is selected). */
void ps2_memcard_set_users_tab_opened(bool opened);

/** True after `ps2_memcard_set_users_tab_opened(true)`. */
bool ps2_memcard_users_tab_opened();

/** Stop further memcard disk reads; subsequent lookups use the init cache only. */
void ps2_memcard_finish_initial_scan();

/**
 * Best-effort match of a catalog title to a memcard folder name
 * (product-code prefix, compact name containment).
 */
std::optional<std::uint64_t> ps2_memcard_bytes_for_game(
    const Ps2MemcardUsage& card,
    std::string_view display_name,
    std::string_view content_stem = {},
    std::string_view serial_or_product = {});

} // namespace archstreamer
