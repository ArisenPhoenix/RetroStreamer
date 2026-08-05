#pragma once

#include "host/catalog_ops.hpp"
#include "host/game_meta_edit_log.hpp"
#include "host/game_meta_store.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace archstreamer {

struct CatalogRepairOptions {
    CatalogFsOptions fs;
    /** Report the plan without touching the DB or the filesystem. */
    bool dry_run = false;
    /** Also align ROM / Meta filenames for rows that no edit ever touched. */
    bool align_all_rows = false;
};

struct CatalogRepairReport {
    std::size_t chains = 0;
    /** Chains whose live identity already matched the last recorded edit. */
    std::size_t already_correct = 0;
    std::size_t repaired = 0;
    std::size_t folded_duplicates = 0;
    std::size_t aligned_files = 0;
    std::size_t missing_rows = 0;
    std::vector<std::string> lines;
};

/**
 * Re-apply the intended identity from game_meta_edits onto the live game_meta rows.
 *
 * A catalog scan can resurrect a pre-edit row (its id survives as a catalog_id alias),
 * which leaves the DB showing the old identity while the edit log still holds the
 * intended one. For each chain of edited ids this:
 * - folds every live duplicate in the chain into one survivor (keeping a real content_path),
 * - re-applies the newest recorded "after" identity to that survivor,
 * - renames the ROM (+ Meta sidecar) and rewrites Meta so the next scan agrees.
 */
CatalogRepairReport repair_catalog_from_edits(
    GameMetaStore& store,
    const GameMetaEditLog& edits,
    const CatalogRepairOptions& options = {});

} // namespace archstreamer
