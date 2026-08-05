#include "host/catalog_repair.hpp"

#include "host/game_catalog_scanner.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>

namespace archstreamer {
namespace {

/** Union-find over game ids so old → new id migrations collapse into one chain. */
class IdGroups {
public:
    void link(const std::string& a, const std::string& b) {
        if (a.empty() || b.empty()) {
            return;
        }
        const auto ra = find(a);
        const auto rb = find(b);
        if (ra != rb) {
            parent_[ra] = rb;
        }
    }

    std::string find(const std::string& id) {
        auto [it, inserted] = parent_.emplace(id, id);
        if (inserted || it->second == id) {
            return id;
        }
        auto root = find(it->second);
        it->second = root;
        return root;
    }

private:
    std::unordered_map<std::string, std::string> parent_;
};

bool same_identity(const GameMetaRecord& a, const GameMetaRecord& b) {
    return a.system_key == b.system_key && a.display_name == b.display_name
        && a.canonical_name == b.canonical_name && a.version == b.version
        && a.language == b.language && a.region == b.region
        && a.content_stem == b.content_stem;
}

std::string identity_label(const GameMetaRecord& row) {
    return catalog_label_for(row.display_name, row.version);
}

struct ChainPlan {
    GameMetaEditRecord entry;
    std::vector<GameMetaRecord> live_rows;
    std::string survivor_id;
    std::string content_path;
    std::filesystem::path planned_rom;
    bool aligned = false;
    bool path_conflict = false;
};

} // namespace

CatalogRepairReport repair_catalog_from_edits(
    GameMetaStore& store,
    const GameMetaEditLog& edits,
    const CatalogRepairOptions& options) {
    CatalogRepairReport report;
    if (!store.ready()) {
        report.lines.push_back("game_meta DB is not open");
        return report;
    }
    if (!edits.ready()) {
        report.lines.push_back("game_meta_edits DB is not open");
        return report;
    }

    auto history = edits.list_edits(100000);
    std::sort(history.begin(), history.end(), [](const auto& a, const auto& b) {
        return a.edit_id < b.edit_id;
    });

    IdGroups groups;
    for (const auto& entry : history) {
        groups.link(entry.old_game_id, entry.new_game_id);
    }

    // Newest recorded "after" per chain is the intended state.
    std::map<std::string, GameMetaEditRecord> intended;
    for (const auto& entry : history) {
        const auto root = groups.find(
            entry.new_game_id.empty() ? entry.old_game_id : entry.new_game_id);
        intended[root] = entry;
    }

    // Live rows grouped by the same chains.
    std::map<std::string, std::vector<GameMetaRecord>> live;
    for (auto& row : store.list_games()) {
        const auto root = groups.find(row.game_id);
        if (intended.count(root) != 0) {
            live[root].push_back(std::move(row));
        }
    }

    std::vector<ChainPlan> plans;
    plans.reserve(intended.size());
    for (const auto& [root, entry] : intended) {
        ChainPlan plan;
        plan.entry = entry;
        const auto& target = entry.after;
        auto found = live.find(root);
        if (found == live.end() || found->second.empty()) {
            ++report.missing_rows;
            report.lines.push_back(
                "no live row for '" + identity_label(target) + "' (edit #"
                + std::to_string(entry.edit_id) + ") — ROM may be gone; skipped");
            continue;
        }
        plan.live_rows = found->second;

        auto survivor = std::find_if(
            plan.live_rows.begin(), plan.live_rows.end(), [&](const GameMetaRecord& row) {
                return row.game_id == target.game_id;
            });
        if (survivor == plan.live_rows.end()) {
            survivor = std::find_if(
                plan.live_rows.begin(), plan.live_rows.end(), [](const GameMetaRecord& row) {
                    return !row.content_path.empty();
                });
        }
        if (survivor == plan.live_rows.end()) {
            survivor = plan.live_rows.begin();
        }

        plan.survivor_id = survivor->game_id;
        plan.content_path = survivor->content_path;
        if (plan.content_path.empty()) {
            for (const auto& row : plan.live_rows) {
                if (!row.content_path.empty()) {
                    plan.content_path = row.content_path;
                    break;
                }
            }
        }
        plan.aligned = same_identity(*survivor, target)
            && survivor->game_id == target.game_id && plan.live_rows.size() == 1;

        GameMetaRecord planned = target;
        planned.content_path = plan.content_path;
        plan.planned_rom = planned_rom_rename_target(planned);
        plans.push_back(std::move(plan));
    }

    // True conflict = two distinct identities (name+version → different game_id)
    // mapping to the exact same file path. Same title with different versions is fine.
    std::map<std::string, std::vector<std::string>> path_claimants;
    for (const auto& plan : plans) {
        if (plan.planned_rom.empty()) {
            continue;
        }
        path_claimants[plan.planned_rom.lexically_normal().string()].push_back(
            plan.entry.after.game_id);
    }
    std::set<std::string> conflicted_ids;
    for (const auto& [path, ids] : path_claimants) {
        std::set<std::string> unique(ids.begin(), ids.end());
        if (unique.size() < 2) {
            continue;
        }
        report.lines.push_back(
            "ROM path conflict (same name AND version): " + path);
        for (const auto& id : unique) {
            conflicted_ids.insert(id);
        }
    }
    for (auto& plan : plans) {
        if (conflicted_ids.count(plan.entry.after.game_id) != 0) {
            plan.path_conflict = true;
        }
    }

    std::set<std::string> touched_ids;
    report.chains = plans.size();

    for (const auto& plan : plans) {
        const auto& target = plan.entry.after;

        for (const auto& row : plan.live_rows) {
            if (row.game_id == plan.survivor_id) {
                continue;
            }
            ++report.folded_duplicates;
            report.lines.push_back(
                "fold duplicate " + row.game_id + " ('" + identity_label(row) + "') → "
                + plan.survivor_id);
            if (options.dry_run) {
                continue;
            }
            (void)store.migrate_user_games_game_id(row.game_id, plan.survivor_id);
            (void)store.migrate_play_modes_game_id(row.game_id, plan.survivor_id);
            (void)store.reassign_aliases_game_id(row.game_id, plan.survivor_id);
            (void)store.delete_game(row.game_id);
            (void)store.upsert_alias(
                game_meta_alias::kCatalogId, row.game_id, {}, plan.survivor_id);
            (void)store.upsert_alias(
                game_meta_alias::kCatalogId, row.game_id, target.system_key, plan.survivor_id);
        }

        if (plan.aligned) {
            ++report.already_correct;
            if (!plan.planned_rom.empty()
                && plan.content_path != plan.planned_rom.lexically_normal().string()) {
                report.lines.push_back(
                    "align files for '" + identity_label(target) + "'"
                    + (plan.path_conflict ? " (ROM rename skipped: path conflict)" : ""));
                report.lines.push_back("  ROM → " + plan.planned_rom.string());
            }
        } else {
            auto survivor_it = std::find_if(
                plan.live_rows.begin(),
                plan.live_rows.end(),
                [&](const GameMetaRecord& row) { return row.game_id == plan.survivor_id; });
            const auto& from = survivor_it != plan.live_rows.end() ? *survivor_it : target;
            report.lines.push_back(
                "repair '" + identity_label(from) + "' → '" + identity_label(target)
                + "' (edit #" + std::to_string(plan.entry.edit_id) + ")");
            if (!plan.planned_rom.empty()
                && plan.content_path != plan.planned_rom.lexically_normal().string()) {
                report.lines.push_back(
                    "  ROM → " + plan.planned_rom.string()
                    + (plan.path_conflict ? " (skipped: path conflict)" : ""));
            }
        }

        if (options.dry_run) {
            touched_ids.insert(target.game_id);
            continue;
        }

        GameMetaRecord edited = target;
        edited.content_path = plan.content_path;
        auto fs = options.fs;
        if (plan.path_conflict) {
            fs.rename_rom = false;
        }
        const auto result = apply_game_meta_edit(
            store,
            plan.survivor_id,
            edited,
            fs,
            "repair",
            "replay intended identity from edit #" + std::to_string(plan.entry.edit_id));
        if (!result.ok) {
            report.lines.push_back(
                "  failed: " + result.message + " (" + plan.survivor_id + ")");
            continue;
        }
        if (!plan.aligned) {
            ++report.repaired;
        }
        for (const auto& effect : result.effects) {
            report.lines.push_back("  " + effect);
            if (effect.rfind("renamed: ", 0) == 0) {
                ++report.aligned_files;
            }
        }
        touched_ids.insert(result.new_game_id);
    }

    if (!options.align_all_rows) {
        return report;
    }

    for (const auto& row : store.list_games()) {
        if (touched_ids.count(row.game_id) != 0 || row.content_path.empty()) {
            continue;
        }
        if (options.dry_run) {
            const auto planned = planned_rom_rename_target(row);
            if (!planned.empty()
                && planned.lexically_normal().string()
                    != std::filesystem::path{row.content_path}.lexically_normal().string()) {
                report.lines.push_back(
                    "would align '" + identity_label(row) + "' → " + planned.string());
            }
            continue;
        }
        const auto result = align_game_files_to_identity(store, row.game_id, options.fs);
        for (const auto& effect : result.effects) {
            report.lines.push_back("  " + effect);
            if (effect.rfind("renamed: ", 0) == 0) {
                ++report.aligned_files;
            }
        }
    }
    return report;
}

} // namespace archstreamer
