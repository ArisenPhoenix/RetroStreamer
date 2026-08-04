#include "host/save_active_sessions.hpp"

#include "host/game_meta_store.hpp"
#include "host/switch_save_share.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
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

bool looks_like_title_id_leaf(std::string_view value) {
    if (value.size() != 16) {
        return false;
    }
    for (char ch : value) {
        if (!std::isxdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    return to_lower(std::string(value)).rfind("0100", 0) == 0;
}

std::optional<GameMetaRecord> resolve_active_meta(
    const GameMetaStore& meta,
    const ActiveSaveSession& active) {
    if (!active.game_id.empty()) {
        if (auto row = meta.resolve(active.game_id, active.system_key)) {
            return row;
        }
    }
    if (!active.content_path.empty()) {
        const auto stem = std::filesystem::path(active.content_path).stem().string();
        if (auto row = meta.resolve(stem, active.system_key)) {
            return row;
        }
        if (looks_like_title_id_leaf(stem)) {
            if (auto row = meta.resolve(normalize_switch_title_id(stem), "switch")) {
                return row;
            }
        }
    }
    if (!active.display_name.empty()) {
        if (auto row = meta.resolve(active.display_name, active.system_key)) {
            return row;
        }
    }
    return std::nullopt;
}

std::optional<GameMetaRecord> resolve_save_meta(
    const GameMetaStore& meta,
    const std::string& system_key,
    const std::string& display_name,
    const std::filesystem::path& primary_path,
    std::string_view game_key = {}) {
    if (is_ps2_meta_game_key(game_key)) {
        const auto id = game_id_from_ps2_meta_key(game_key);
        if (auto row = meta.find_by_id(id)) {
            return row;
        }
        return meta.resolve(id, "ps2");
    }
    if (system_key == "switch") {
        const auto leaf = primary_path.filename().string();
        if (looks_like_title_id_leaf(leaf)) {
            if (auto row = meta.resolve(normalize_switch_title_id(leaf), "switch")) {
                return row;
            }
        }
        if (auto row = meta.resolve(leaf, "switch")) {
            return row;
        }
    }
    if (game_key.size() > 5 && game_key.substr(0, 5) == "file:") {
        if (auto row = meta.resolve(primary_path.stem().string(), system_key)) {
            return row;
        }
    }
    if (auto row = meta.resolve(primary_path.stem().string(), system_key)) {
        return row;
    }
    if (!display_name.empty()) {
        if (auto row = meta.resolve(display_name, system_key)) {
            return row;
        }
    }
    return std::nullopt;
}

/** Exact path/stem equality only — used when the meta DB has no row yet. */
bool exact_path_match(
    const std::filesystem::path& primary_path,
    const ActiveSaveSession& active) {
    if (active.content_path.empty()) {
        return false;
    }
    const auto content = std::filesystem::path(active.content_path);
    const auto content_stem = to_lower(content.stem().string());
    const auto content_file = to_lower(content.filename().string());
    if (!content_stem.empty() && content_stem == to_lower(primary_path.stem().string())) {
        return true;
    }
    if (!content_file.empty() && content_file == to_lower(primary_path.filename().string())) {
        return true;
    }
    if (looks_like_title_id_leaf(primary_path.filename().string())
        && looks_like_title_id_leaf(content_stem)
        && normalize_switch_title_id(primary_path.filename().string())
            == normalize_switch_title_id(content_stem)) {
        return true;
    }
    return false;
}

std::filesystem::path slot_status_path(const std::filesystem::path& save_root, int slot_index) {
    return active_save_sessions_directory(save_root)
        / ("slot-" + std::to_string(slot_index) + ".json");
}

std::filesystem::path slot_stop_path(const std::filesystem::path& save_root, int slot_index) {
    return active_save_sessions_directory(save_root)
        / ("stop-slot-" + std::to_string(slot_index));
}

std::string connected_key(std::uint32_t client_id, int slot_index) {
    if (slot_index < 0) {
        return "lobby-" + std::to_string(client_id);
    }
    return "slot-" + std::to_string(slot_index) + "-" + std::to_string(client_id);
}

std::filesystem::path connected_path(
    const std::filesystem::path& save_root,
    std::uint32_t client_id,
    int slot_index) {
    return active_save_sessions_directory(save_root)
        / ("connected-" + connected_key(client_id, slot_index) + ".json");
}

std::filesystem::path disconnect_path(
    const std::filesystem::path& save_root,
    std::uint32_t client_id,
    int slot_index) {
    return active_save_sessions_directory(save_root)
        / ("disconnect-" + connected_key(client_id, slot_index));
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
    std::filesystem::remove(slot_stop_path(save_root, slot_index), ec);
    clear_connected_clients_for_slot(save_root, slot_index);
}

void request_active_session_stop(
    const std::filesystem::path& save_root,
    int slot_index,
    std::string_view reason) {
    if (save_root.empty() || slot_index < 0) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(active_save_sessions_directory(save_root), ec);
    nlohmann::json json{
        {"reason", reason.empty() ? "kicked" : std::string(reason)},
        {"slot_index", slot_index},
    };
    std::ofstream out(slot_stop_path(save_root, slot_index), std::ios::trunc);
    if (!out) {
        return;
    }
    out << json.dump(2) << '\n';
}

std::optional<std::string> take_active_session_stop_request(
    const std::filesystem::path& save_root,
    int slot_index) {
    if (save_root.empty() || slot_index < 0) {
        return std::nullopt;
    }
    const auto path = slot_stop_path(save_root, slot_index);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return std::nullopt;
    }
    std::string reason = "kicked";
    {
        std::ifstream in(path);
        if (in) {
            try {
                const auto json = nlohmann::json::parse(in);
                reason = json.value("reason", reason);
            } catch (const nlohmann::json::exception&) {
            }
        }
    }
    std::filesystem::remove(path, ec);
    return reason;
}

void publish_connected_client(
    const std::filesystem::path& save_root,
    const ConnectedClientPresence& client) {
    if (save_root.empty() || client.username.empty() || client.client_id == 0) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(active_save_sessions_directory(save_root), ec);
    nlohmann::json json{
        {"username", client.username},
        {"client_id", client.client_id},
        {"slot_index", client.slot_index},
        {"game_id", client.game_id},
        {"phase", client.phase.empty() ? (client.slot_index < 0 ? "lobby" : "session")
                                       : client.phase},
        {"seated", client.seated},
    };
    std::ofstream out(connected_path(save_root, client.client_id, client.slot_index), std::ios::trunc);
    if (!out) {
        return;
    }
    out << json.dump(2) << '\n';
}

void clear_connected_client(
    const std::filesystem::path& save_root,
    std::uint32_t client_id,
    int slot_index) {
    if (save_root.empty() || client_id == 0) {
        return;
    }
    std::error_code ec;
    std::filesystem::remove(connected_path(save_root, client_id, slot_index), ec);
    std::filesystem::remove(disconnect_path(save_root, client_id, slot_index), ec);
}

void clear_connected_clients_for_slot(
    const std::filesystem::path& save_root,
    int slot_index) {
    if (save_root.empty()) {
        return;
    }
    std::error_code ec;
    const auto dir = active_save_sessions_directory(save_root);
    if (!std::filesystem::is_directory(dir, ec)) {
        return;
    }
    const std::string prefix = slot_index < 0
        ? "connected-lobby-"
        : ("connected-slot-" + std::to_string(slot_index) + "-");
    const std::string disconnect_prefix = slot_index < 0
        ? "disconnect-lobby-"
        : ("disconnect-slot-" + std::to_string(slot_index) + "-");
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (name.rfind(prefix, 0) == 0 || name.rfind(disconnect_prefix, 0) == 0) {
            std::filesystem::remove(entry.path(), ec);
        }
    }
}

std::vector<ConnectedClientPresence> list_connected_clients(
    const std::filesystem::path& save_root) {
    std::vector<ConnectedClientPresence> out;
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
        if (name.rfind("connected-", 0) != 0 || entry.path().extension() != ".json") {
            continue;
        }
        std::ifstream in(entry.path());
        if (!in) {
            continue;
        }
        try {
            const auto json = nlohmann::json::parse(in);
            ConnectedClientPresence client;
            client.username = json.value("username", "");
            client.client_id = json.value("client_id", 0u);
            client.slot_index = json.value("slot_index", -1);
            client.game_id = json.value("game_id", "");
            client.phase = json.value("phase", "");
            client.seated = json.value("seated", false);
            if (!client.username.empty() && client.client_id != 0) {
                out.push_back(std::move(client));
            }
        } catch (const nlohmann::json::exception&) {
            continue;
        }
    }
    std::sort(
        out.begin(),
        out.end(),
        [](const ConnectedClientPresence& a, const ConnectedClientPresence& b) {
            if (a.username != b.username) {
                return a.username < b.username;
            }
            if (a.slot_index != b.slot_index) {
                return a.slot_index < b.slot_index;
            }
            return a.client_id < b.client_id;
        });
    return out;
}

void request_connected_client_disconnect(
    const std::filesystem::path& save_root,
    std::uint32_t client_id,
    int slot_index,
    std::string_view reason) {
    if (save_root.empty() || client_id == 0) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(active_save_sessions_directory(save_root), ec);
    nlohmann::json json{
        {"reason", reason.empty() ? "kicked" : std::string(reason)},
        {"client_id", client_id},
        {"slot_index", slot_index},
    };
    std::ofstream out(disconnect_path(save_root, client_id, slot_index), std::ios::trunc);
    if (!out) {
        return;
    }
    out << json.dump(2) << '\n';
}

std::optional<std::string> take_connected_client_disconnect_request(
    const std::filesystem::path& save_root,
    std::uint32_t client_id,
    int slot_index) {
    if (save_root.empty() || client_id == 0) {
        return std::nullopt;
    }
    const auto path = disconnect_path(save_root, client_id, slot_index);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return std::nullopt;
    }
    std::string reason = "kicked";
    {
        std::ifstream in(path);
        if (in) {
            try {
                const auto json = nlohmann::json::parse(in);
                reason = json.value("reason", reason);
            } catch (const nlohmann::json::exception&) {
            }
        }
    }
    std::filesystem::remove(path, ec);
    return reason;
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
        // stop-slot-N is not a status file (no .json).
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
    const ActiveSaveSession& active,
    std::string_view game_key) {
    if (to_lower(username) != to_lower(active.username)) {
        return false;
    }
    if (!active.system_key.empty() && !system_key.empty()
        && to_lower(system_key) != to_lower(active.system_key)) {
        return false;
    }

    try {
        GameMetaStore meta;
        if (meta.ready()) {
            const auto active_meta = resolve_active_meta(meta, active);
            const auto save_meta =
                resolve_save_meta(meta, system_key, display_name, primary_path, game_key);
            if (active_meta && save_meta && active_meta->game_id == save_meta->game_id) {
                return true;
            }
        }
    } catch (...) {
        // Fall through to exact path match.
    }

    return exact_path_match(primary_path, active);
}

std::optional<std::string> best_active_game_key(
    const std::vector<SaveGameEntry>& games,
    const ActiveSaveSession& active) {
    std::vector<const SaveGameEntry*> system_games;
    for (const auto& game : games) {
        if (to_lower(game.username) != to_lower(active.username)) {
            continue;
        }
        if (active.system_key.empty()
            || to_lower(game.system_key) == to_lower(active.system_key)) {
            system_games.push_back(&game);
        }
    }

    // Prefer a meta-resolved identity match.
    try {
        GameMetaStore meta;
        if (meta.ready()) {
            if (const auto active_meta = resolve_active_meta(meta, active)) {
                for (const auto* game : system_games) {
                    if (const auto save_meta = resolve_save_meta(
                            meta,
                            game->system_key,
                            game->display_name,
                            game->primary_path,
                            game->game_key)) {
                        if (save_meta->game_id == active_meta->game_id) {
                            return game->game_key;
                        }
                    }
                }
            }
        }
    } catch (...) {
    }

    for (const auto* game : system_games) {
        if (save_entry_is_active(
                game->username,
                game->system_key,
                game->display_name,
                game->primary_path,
                active,
                game->game_key)) {
            return game->game_key;
        }
    }
    // Legacy memcard rows (ps2:Mcd*.ps2) must never receive Active.
    if (to_lower(active.system_key) == "ps2") {
        for (const auto* game : system_games) {
            if (is_ps2_meta_game_key(game->game_key)) {
                // Already tried meta match above; do not invent Active on cards.
                continue;
            }
        }
        const bool only_meta = !system_games.empty()
            && std::all_of(system_games.begin(), system_games.end(), [](const SaveGameEntry* g) {
                   return is_ps2_meta_game_key(g->game_key);
               });
        if (!only_meta) {
            return std::nullopt;
        }
    }
    if (system_games.size() == 1) {
        return system_games.front()->game_key;
    }
    return std::nullopt;
}

} // namespace archstreamer
