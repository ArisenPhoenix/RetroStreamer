#include "host/save_active_sessions.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <string_view>
#include <system_error>

namespace archstreamer {
namespace {

std::string to_lower(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string normalize_match_key(std::string value) {
    value = to_lower(std::move(value));
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
            out.push_back(ch);
        }
    }
    return out;
}

bool names_fuzzy_match(std::string_view a, std::string_view b) {
    const auto na = normalize_match_key(std::string(a));
    const auto nb = normalize_match_key(std::string(b));
    if (na.empty() || nb.empty()) {
        return false;
    }
    if (na == nb) {
        return true;
    }
    // "Pokemon Sword" vs "Pokemon Sword 132" / title-id display fallbacks.
    return na.find(nb) != std::string::npos || nb.find(na) != std::string::npos;
}

std::filesystem::path slot_status_path(const std::filesystem::path& save_root, int slot_index) {
    return active_save_sessions_directory(save_root)
        / ("slot-" + std::to_string(slot_index) + ".json");
}

} // namespace

std::filesystem::path active_save_sessions_directory(const std::filesystem::path& save_root) {
    return save_root / ".archstreamer_active";
}

void publish_active_save_session(
    const std::filesystem::path& save_root,
    const ActiveSaveSession& session) {
    if (save_root.empty() || session.slot_index < 0 || session.username.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(active_save_sessions_directory(save_root), ec);
    nlohmann::json json{
        {"username", session.username},
        {"game_id", session.game_id},
        {"system_key", session.system_key},
        {"display_name", session.display_name},
        {"content_path", session.content_path},
        {"slot_index", session.slot_index},
    };
    const auto path = slot_status_path(save_root, session.slot_index);
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        return;
    }
    out << json.dump(2) << '\n';
}

void clear_active_save_session(
    const std::filesystem::path& save_root,
    int slot_index) {
    if (save_root.empty() || slot_index < 0) {
        return;
    }
    std::error_code ec;
    std::filesystem::remove(slot_status_path(save_root, slot_index), ec);
}

std::vector<ActiveSaveSession> list_active_save_sessions(
    const std::filesystem::path& save_root) {
    std::vector<ActiveSaveSession> out;
    std::error_code ec;
    const auto dir = active_save_sessions_directory(save_root);
    if (!std::filesystem::is_directory(dir, ec)) {
        return out;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (name.rfind("slot-", 0) != 0 || entry.path().extension() != ".json") {
            continue;
        }
        std::ifstream in(entry.path());
        if (!in) {
            continue;
        }
        try {
            const auto json = nlohmann::json::parse(in);
            ActiveSaveSession session;
            session.username = json.value("username", "");
            session.game_id = json.value("game_id", "");
            session.system_key = json.value("system_key", "");
            session.display_name = json.value("display_name", "");
            session.content_path = json.value("content_path", "");
            session.slot_index = json.value("slot_index", -1);
            if (!session.username.empty()) {
                out.push_back(std::move(session));
            }
        } catch (const nlohmann::json::exception&) {
            continue;
        }
    }
    std::sort(out.begin(), out.end(), [](const ActiveSaveSession& a, const ActiveSaveSession& b) {
        if (a.username != b.username) {
            return a.username < b.username;
        }
        return a.slot_index < b.slot_index;
    });
    return out;
}

bool save_entry_is_active(
    const std::string& username,
    const std::string& system_key,
    const std::string& display_name,
    const std::filesystem::path& primary_path,
    const ActiveSaveSession& active) {
    if (to_lower(username) != to_lower(active.username)) {
        return false;
    }
    if (!active.system_key.empty() && !system_key.empty()
        && to_lower(system_key) != to_lower(active.system_key)) {
        return false;
    }

    const auto content_stem = std::filesystem::path(active.content_path).stem().string();
    if (!content_stem.empty()) {
        if (names_fuzzy_match(content_stem, display_name)) {
            return true;
        }
        if (names_fuzzy_match(content_stem, primary_path.stem().string())) {
            return true;
        }
        if (names_fuzzy_match(content_stem, primary_path.filename().string())) {
            return true;
        }
    }
    if (!active.display_name.empty() && names_fuzzy_match(active.display_name, display_name)) {
        return true;
    }
    // Switch title-id leaves: active display may be "Pokemon Sword" while the row
    // is "Switch 0100abf…" — still match when the leaf name itself fuzzy-matches content.
    if (system_key == "switch" && !content_stem.empty()) {
        if (names_fuzzy_match(content_stem, primary_path.filename().string())) {
            return true;
        }
    }
    return false;
}

std::optional<std::string> best_active_game_key(
    const std::vector<SaveGameEntry>& games,
    const ActiveSaveSession& active) {
    std::vector<const SaveGameEntry*> user_games;
    std::vector<const SaveGameEntry*> system_games;
    for (const auto& game : games) {
        if (to_lower(game.username) != to_lower(active.username)) {
            continue;
        }
        user_games.push_back(&game);
        if (active.system_key.empty()
            || to_lower(game.system_key) == to_lower(active.system_key)) {
            system_games.push_back(&game);
        }
    }
    for (const auto* game : system_games) {
        if (save_entry_is_active(
                game->username,
                game->system_key,
                game->display_name,
                game->primary_path,
                active)) {
            return game->game_key;
        }
    }
    // PS2 is memcard-backed — never invent an Active marker on Mcd*.ps2 rows.
    if (to_lower(active.system_key) == "ps2") {
        return std::nullopt;
    }
    if (system_games.size() == 1) {
        return system_games.front()->game_key;
    }
    return std::nullopt;
}

} // namespace archstreamer
