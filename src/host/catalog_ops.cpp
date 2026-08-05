#include "host/catalog_ops.hpp"

#include "common/dlc_paths.hpp"
#include "common/sha256.hpp"
#include "host/game_catalog_scanner.hpp"
#include "host/game_meta_edit_log.hpp"
#include "host/save_manager.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <system_error>
#include <utility>

namespace archstreamer {
namespace {

std::string to_lower_copy(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

bool equals_ci(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) {
            return false;
        }
    }
    return true;
}

bool rename_path_best_effort(
    const std::filesystem::path& from,
    const std::filesystem::path& to,
    std::vector<std::string>& effects) {
    std::error_code ec;
    if (!std::filesystem::exists(from, ec) || ec) {
        return false;
    }
    if (std::filesystem::exists(to, ec) && !ec) {
        effects.push_back(
            "skip rename (target exists): " + from.string() + " → " + to.string());
        return false;
    }
    std::filesystem::create_directories(to.parent_path(), ec);
    ec.clear();
    std::filesystem::rename(from, to, ec);
    if (ec) {
        effects.push_back(
            "rename failed: " + from.string() + " → " + to.string() + " (" + ec.message() + ")");
        return false;
    }
    effects.push_back("renamed: " + from.string() + " → " + to.string());
    return true;
}

void rename_stem_files_in_dir(
    const std::filesystem::path& dir,
    std::string_view old_stem,
    std::string_view new_stem,
    std::vector<std::string>& effects) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return;
    }
    const auto old_l = to_lower_copy(std::string(old_stem));
    const auto new_s = std::string(new_stem);
    std::vector<std::filesystem::path> matches;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        if (to_lower_copy(entry.path().stem().string()) == old_l) {
            matches.push_back(entry.path());
        }
    }
    for (const auto& path : matches) {
        const auto dest = path.parent_path() / (new_s + path.extension().string());
        (void)rename_path_best_effort(path, dest, effects);
    }
}

void rename_content_stem_across_saves(
    const std::filesystem::path& save_root,
    std::string_view old_stem,
    std::string_view new_stem,
    std::vector<std::string>& effects) {
    if (old_stem.empty() || new_stem.empty() || old_stem == new_stem || save_root.empty()) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(save_root, ec)) {
        return;
    }
    for (const auto& user_entry : std::filesystem::directory_iterator(save_root, ec)) {
        if (!user_entry.is_directory(ec)) {
            continue;
        }
        const auto username = user_entry.path().filename().string();
        if (username.empty() || username == "template" || username.front() == '.') {
            continue;
        }
        const auto user_dir = user_entry.path();
        rename_stem_files_in_dir(user_dir / "saves", old_stem, new_stem, effects);
        rename_stem_files_in_dir(user_dir / "states", old_stem, new_stem, effects);
        (void)rename_path_best_effort(
            user_dir / "switch" / "saves" / std::string(old_stem),
            user_dir / "switch" / "saves" / std::string(new_stem),
            effects);
        // Legacy per-user addon trees (pre–global DLC).
        (void)rename_path_best_effort(
            user_dir / "switch" / "addons" / std::string(old_stem),
            user_dir / "switch" / "addons" / std::string(new_stem),
            effects);
    }
}

void rename_dlc_game_directory(
    const std::filesystem::path& dlc_root,
    std::string_view old_system_key,
    std::string_view new_system_key,
    std::string_view old_stem,
    std::string_view new_stem,
    std::vector<std::string>& effects) {
    if (dlc_root.empty() || old_stem.empty() || new_stem.empty()) {
        return;
    }
    const auto from = catalog_dlc_game_directory(dlc_root, old_system_key, old_stem);
    const auto to = catalog_dlc_game_directory(dlc_root, new_system_key, new_stem);
    if (from.empty() || to.empty() || from == to) {
        return;
    }
    (void)rename_path_best_effort(from, to, effects);
}

void rename_art_asset_key(
    const std::filesystem::path& art_root,
    std::string_view old_key,
    std::string_view new_key,
    std::vector<std::string>& effects) {
    if (old_key.empty() || new_key.empty() || old_key == new_key || art_root.empty()) {
        return;
    }
    (void)rename_path_best_effort(
        art_root / std::filesystem::path{std::string(old_key)},
        art_root / std::filesystem::path{std::string(new_key)},
        effects);
}

/** Strip one trailing "(…)" group if it looks like region/revision noise. */
bool strip_one_paren_tag(std::string& name, std::string& captured) {
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
        name.pop_back();
    }
    if (name.size() < 3 || name.back() != ')') {
        return false;
    }
    const auto open = name.rfind(" (");
    if (open == std::string::npos) {
        return false;
    }
    captured = name.substr(open + 2, name.size() - open - 3);
    name.resize(open);
    return true;
}

bool looks_like_region_tag(std::string_view tag) {
    const auto lower = to_lower_copy(std::string(tag));
    static constexpr const char* kHints[] = {
        "usa", "europe", "eur", "japan", "jpn", "world", "australia", "france",
        "germany", "spain", "italy", "korea", "china", "taiwan", "brazil",
        "en", "en-us", "en-gb", "fr", "de", "es", "it", "nl", "pt", "ru", "zh", "ja",
    };
    for (const auto* hint : kHints) {
        if (lower.find(hint) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool looks_like_revision_tag(std::string_view tag) {
    const auto lower = to_lower_copy(std::string(tag));
    return lower.rfind("rev", 0) == 0 || lower.rfind("revision", 0) == 0;
}

} // namespace

GameMetaRecord recompute_game_meta_identity(GameMetaRecord row) {
    row.system_key = canonical_token(row.system_key);
    row.canonical_name = canonical_token(
        row.canonical_name.empty() ? row.display_name : row.canonical_name);
    row.version = canonical_token(row.version.empty() ? "unknown" : row.version);
    row.language = canonical_token(row.language.empty() ? "en" : row.language);
    row.region = canonical_token(row.region.empty() ? "unknown" : row.region);
    row.identity_key = identity_key_for(
        row.system_key,
        row.canonical_name,
        row.version,
        row.language,
        row.region);
    row.game_id = sha256_hex(row.identity_key);
    row.asset_key = asset_key_for(
        row.system_key,
        row.canonical_name,
        row.language,
        row.region,
        row.version);
    row.updated_at = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    return row;
}

CatalogOpResult apply_game_meta_edit(
    GameMetaStore& store,
    std::string_view current_game_id,
    const GameMetaRecord& edited,
    const CatalogFsOptions& fs,
    std::string_view op,
    std::string_view note) {
    CatalogOpResult result;
    result.old_game_id = std::string(current_game_id);
    if (!store.ready()) {
        result.message = "game_meta DB is not open";
        return result;
    }
    auto existing = store.find_by_id(current_game_id);
    if (!existing) {
        result.message = "game_id not found";
        return result;
    }
    const GameMetaRecord before = *existing;

    GameMetaRecord next = *existing;
    next.system_key = edited.system_key;
    next.system_name = edited.system_name;
    next.display_name = edited.display_name;
    next.canonical_name = edited.canonical_name;
    next.core_name = edited.core_name;
    next.version = edited.version;
    next.language = edited.language;
    next.region = edited.region;
    next.content_stem = edited.content_stem;
    if (!edited.content_path.empty()) {
        next.content_path = edited.content_path;
    }
    if (!edited.source.empty()) {
        next.source = edited.source;
    }
    next = recompute_game_meta_identity(std::move(next));
    result.new_game_id = next.game_id;

    const auto old_stem = existing->content_stem;
    const auto new_stem = next.content_stem;
    const auto old_asset = existing->asset_key;
    const auto new_asset = next.asset_key;
    const auto old_system = existing->system_key;
    const auto new_system = next.system_key;
    const bool id_changed = next.game_id != existing->game_id;

    if (id_changed) {
        if (store.find_by_id(next.game_id)) {
            result.message =
                "recomputed game_id already exists — resolve the collision before editing";
            return result;
        }
        if (!store.upsert(next)) {
            result.message = "failed to insert migrated game_meta row";
            return result;
        }
        const auto aliases_moved =
            store.reassign_aliases_game_id(existing->game_id, next.game_id);
        result.effects.push_back(
            "reassigned " + std::to_string(aliases_moved) + " alias row(s) to new game_id");
        const auto users_moved =
            store.migrate_user_games_game_id(existing->game_id, next.game_id);
        result.effects.push_back(
            "migrated " + std::to_string(users_moved) + " user_games row(s)");
        if (store.migrate_play_modes_game_id(existing->game_id, next.game_id)) {
            result.effects.push_back("migrated game_play_modes row");
        }
        (void)store.upsert_alias(
            game_meta_alias::kCatalogId, existing->game_id, {}, next.game_id);
        (void)store.upsert_alias(
            game_meta_alias::kCatalogId, existing->game_id, next.system_key, next.game_id);
        result.effects.push_back(
            "kept old game_id as catalog_id alias → " + next.game_id);
        if (!store.delete_game(existing->game_id)) {
            result.message = "inserted new row but failed to delete old game_meta row";
            return result;
        }
        result.effects.push_back("deleted old game_meta row " + existing->game_id);
    } else {
        if (!store.upsert(next)) {
            result.message = "failed to update game_meta row";
            return result;
        }
        result.effects.push_back("updated game_meta row (game_id unchanged)");
    }

    // Preserve previous stem as an alias so unmatched saves still resolve.
    if (!old_stem.empty() && old_stem != new_stem) {
        (void)store.upsert_alias(
            game_meta_alias::kContentStem, old_stem, next.system_key, next.game_id);
        (void)store.upsert_alias(game_meta_alias::kContentStem, old_stem, {}, next.game_id);
        result.effects.push_back("kept old content_stem as alias: " + old_stem);
    }

    store.rebuild_standard_aliases(next);
    // Re-apply old-id + old-stem aliases after rebuild wiped them.
    if (id_changed) {
        (void)store.upsert_alias(
            game_meta_alias::kCatalogId, existing->game_id, {}, next.game_id);
        (void)store.upsert_alias(
            game_meta_alias::kCatalogId, existing->game_id, next.system_key, next.game_id);
    }
    if (!old_stem.empty() && old_stem != new_stem) {
        (void)store.upsert_alias(
            game_meta_alias::kContentStem, old_stem, next.system_key, next.game_id);
        (void)store.upsert_alias(game_meta_alias::kContentStem, old_stem, {}, next.game_id);
    }
    result.effects.push_back("rebuilt standard aliases");

    if (fs.apply_filesystem) {
        if (old_stem != new_stem) {
            rename_content_stem_across_saves(fs.save_root, old_stem, new_stem, result.effects);
        }
        if (old_asset != new_asset) {
            rename_art_asset_key(fs.art_root, old_asset, new_asset, result.effects);
        }
        auto dlc = fs.dlc_root;
        if (dlc.empty() && !fs.rom_root.empty()) {
            dlc = default_dlc_root_for(fs.rom_root);
        }
        if (dlc.empty()) {
            dlc = resolve_dlc_root();
        }
        if (old_stem != new_stem || old_system != new_system) {
            rename_dlc_game_directory(
                dlc, old_system, new_system, old_stem, new_stem, result.effects);
        }
    }

    // Keep Meta JSON in sync so directory scans cannot resurrect stale identities.
    if (next.content_path.empty()) {
        next.content_path = existing->content_path;
    }
    if (store.write_meta_sidecar(next)) {
        result.effects.push_back("wrote Meta sidecar for " + next.display_name);
    }

    result.ok = true;
    result.message = id_changed
        ? ("migrated game_id → " + next.game_id)
        : "updated game_meta";

    GameMetaEditLog edit_log;
    result.edit_id = edit_log.record(op, before, next, result.effects, note);
    if (result.edit_id > 0) {
        result.effects.push_back(
            "recorded edit #" + std::to_string(result.edit_id) + " → "
            + edit_log.path().string());
    }
    return result;
}

CatalogOpResult rollback_game_meta_edit(
    GameMetaStore& store,
    std::int64_t edit_id,
    const CatalogFsOptions& fs) {
    CatalogOpResult result;
    GameMetaEditLog edit_log;
    auto entry = edit_log.find_edit(edit_id);
    if (!entry) {
        result.message = "edit_id not found in game_meta_edits";
        return result;
    }
    // Prefer restoring from the surviving id after the original edit.
    std::string current_id = entry->new_game_id;
    if (!store.find_by_id(current_id)) {
        current_id = entry->old_game_id;
    }
    if (!store.find_by_id(current_id)) {
        // Alias resolve: old id may still point at the migrated row.
        if (auto resolved = store.resolve(entry->old_game_id)) {
            current_id = resolved->game_id;
        } else if (auto resolved = store.resolve(entry->new_game_id)) {
            current_id = resolved->game_id;
        } else {
            result.message = "neither old nor new game_id is present for rollback";
            return result;
        }
    }
    return apply_game_meta_edit(
        store,
        current_id,
        entry->before,
        fs,
        "rollback",
        "rollback of edit #" + std::to_string(edit_id));
}

CatalogOpResult add_game_meta_alias(
    GameMetaStore& store,
    std::string_view game_id,
    std::string_view alias_kind,
    std::string_view alias_value,
    std::string_view system_key) {
    CatalogOpResult result;
    result.old_game_id = std::string(game_id);
    result.new_game_id = result.old_game_id;
    if (!store.ready()) {
        result.message = "game_meta DB is not open";
        return result;
    }
    if (!store.find_by_id(game_id)) {
        result.message = "game_id not found";
        return result;
    }
    if (alias_kind.empty() || alias_value.empty()) {
        result.message = "alias_kind and alias_value are required";
        return result;
    }
    if (!store.upsert_alias(alias_kind, alias_value, system_key, game_id)) {
        result.message = "failed to upsert alias";
        return result;
    }
    result.ok = true;
    result.message = "alias upserted";
    result.effects.push_back(
        std::string(alias_kind) + "=" + std::string(alias_value)
        + (system_key.empty() ? " (global)" : (" @" + std::string(system_key))));
    return result;
}

CatalogOpResult delete_game_meta_alias(
    GameMetaStore& store,
    std::string_view alias_kind,
    std::string_view alias_value,
    std::string_view system_key) {
    CatalogOpResult result;
    if (!store.ready()) {
        result.message = "game_meta DB is not open";
        return result;
    }
    if (!store.delete_alias(alias_kind, alias_value, system_key)) {
        result.message = "failed to delete alias (missing?)";
        return result;
    }
    result.ok = true;
    result.message = "alias deleted";
    return result;
}

CatalogOpResult delete_game_meta_entry(
    GameMetaStore& store,
    std::string_view game_id,
    bool remove_user_games) {
    CatalogOpResult result;
    result.old_game_id = std::string(game_id);
    if (!store.ready()) {
        result.message = "game_meta DB is not open";
        return result;
    }
    if (!store.find_by_id(game_id)) {
        result.message = "game_id not found";
        return result;
    }
    const auto aliases = store.delete_aliases_for_game(game_id);
    result.effects.push_back("deleted " + std::to_string(aliases) + " alias row(s)");
    if (remove_user_games) {
        const auto users = store.remove_user_games_for_game_id(game_id);
        result.effects.push_back("deleted " + std::to_string(users) + " user_games row(s)");
    }
    if (!store.delete_game(game_id)) {
        result.message = "failed to delete game_meta row";
        return result;
    }
    result.ok = true;
    result.message = "game_meta entry deleted (saves/art left on disk)";
    return result;
}

CatalogOpResult normalize_game_meta_names(
    GameMetaStore& store,
    std::string_view game_id,
    const CatalogFsOptions& fs,
    bool rederive_canonical_from_display) {
    CatalogOpResult result;
    result.old_game_id = std::string(game_id);
    if (!store.ready()) {
        result.message = "game_meta DB is not open";
        return result;
    }
    auto existing = store.find_by_id(game_id);
    if (!existing) {
        result.message = "game_id not found";
        return result;
    }

    GameMetaRecord edited = *existing;
    std::string display = edited.display_name;
    std::string stem = edited.content_stem;
    std::vector<std::string> region_parts;
    std::string revision;

    auto consume_tags = [&](std::string& name) {
        for (;;) {
            std::string tag;
            if (!strip_one_paren_tag(name, tag)) {
                break;
            }
            if (looks_like_revision_tag(tag)) {
                revision = tag;
                continue;
            }
            if (looks_like_region_tag(tag)) {
                region_parts.push_back(tag);
                continue;
            }
            // Unknown paren — put it back.
            name += " (";
            name += tag;
            name += ")";
            break;
        }
        name = sanitize_game_display_name(std::move(name));
    };

    consume_tags(display);
    // content_stem is often already lowercased; still strip paren tags / Version.
    {
        std::string stem_work = stem;
        consume_tags(stem_work);
        stem = to_lower_copy(std::move(stem_work));
    }

    if (!region_parts.empty()) {
        std::string joined;
        for (std::size_t i = 0; i < region_parts.size(); ++i) {
            if (i != 0) {
                joined += ", ";
            }
            joined += region_parts[i];
        }
        edited.region = joined;
        result.effects.push_back("region ← " + joined);
    }
    if (!revision.empty()
        && (edited.version.empty() || edited.version == "unknown")) {
        edited.version = revision;
        result.effects.push_back("version ← " + revision);
    }

    // Lone language tokens sometimes appear as region-like "en".
    if (equals_ci(edited.region, "en") || equals_ci(edited.region, "en-us")
        || equals_ci(edited.region, "en-gb")) {
        if (edited.language.empty() || edited.language == "en") {
            edited.language = edited.region;
        }
        // Keep region as the token too — user can clear if unwanted.
    }

    edited.display_name = display;
    edited.content_stem = stem;
    if (rederive_canonical_from_display) {
        edited.canonical_name = display;
        result.effects.push_back("canonical_name rederived from cleaned display");
    }

    return apply_game_meta_edit(store, game_id, edited, fs, "normalize");
}

} // namespace archstreamer
