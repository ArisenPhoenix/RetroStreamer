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
 * Cached summary of a PS2 memory-card image. Never reads the disk, so it is
 * safe on a UI thread: it yields nullopt until `ps2_memcard_prewarm()` has
 * parsed that path, and callers simply report an unknown size until then.
 */
std::optional<Ps2MemcardUsage> read_ps2_memcard_usage(const std::filesystem::path& path);

/**
 * Parse `paths` into the cache. Multi-MiB images make this slow enough to
 * freeze a UI, so call it from a worker thread. Runs once per process: a
 * second call (or a concurrent one) returns without touching the disk.
 *
 * Soft-fail per image: one that is missing, unreadable, or not in Sony PS2
 * Memory Card Format caches as nullopt rather than failing the pass.
 */
void ps2_memcard_prewarm(const std::vector<std::filesystem::path>& paths);

/** True once a `ps2_memcard_prewarm()` pass has finished. */
bool ps2_memcard_scan_complete();

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
