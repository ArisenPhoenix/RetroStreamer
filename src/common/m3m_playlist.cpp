#include "common/m3m_playlist.hpp"

#include <cctype>
#include <fstream>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace archstreamer {
namespace {

std::string_view trim_view(std::string_view value) {
    while (!value.empty()
           && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) {
        value.remove_prefix(1);
    }
    while (!value.empty()
           && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return value;
}

bool is_comment_or_blank(std::string_view line) {
    line = trim_view(line);
    return line.empty() || line.front() == '#';
}

} // namespace

std::optional<M3mPlaylist> parse_m3m_playlist(
    const std::filesystem::path& m3m_path,
    std::string* error_out) {
    auto fail = [&](std::string message) -> std::optional<M3mPlaylist> {
        if (error_out != nullptr) {
            *error_out = std::move(message);
        }
        return std::nullopt;
    };

    std::ifstream in(m3m_path);
    if (!in) {
        return fail("unreadable .m3m");
    }

    std::unordered_map<std::string, std::string> values;
    std::string line;
    while (std::getline(in, line)) {
        std::string_view view = line;
        if (!view.empty() && view.back() == '\r') {
            view.remove_suffix(1);
        }
        if (is_comment_or_blank(view)) {
            continue;
        }
        view = trim_view(view);
        const auto eq = view.find('=');
        if (eq == std::string_view::npos) {
            return fail("invalid line (expected KEY=value): " + std::string(view));
        }
        auto key = std::string(trim_view(view.substr(0, eq)));
        auto value = std::string(trim_view(view.substr(eq + 1)));
        for (char& ch : key) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        if (key.empty()) {
            return fail("empty key in .m3m");
        }
        values[std::move(key)] = std::move(value);
    }

    auto require = [&](const char* key) -> std::optional<std::string> {
        const auto it = values.find(key);
        if (it == values.end() || it->second.empty()) {
            return std::nullopt;
        }
        return it->second;
    };

    // Required for Switch save / addon linking and launch ROM resolution.
    const auto title_id = require("TITLE_ID");
    if (!title_id) {
        return fail("missing TITLE_ID (Nintendo title id used by Yuzu/Ryujinx for saves)");
    }
    const auto rom = require("ROM");
    if (!rom) {
        return fail("missing ROM (basename or relative path of the .xci/.nsp to launch)");
    }
    const auto patch_title_id = require("PATCH_TITLE_ID");
    if (!patch_title_id) {
        return fail("missing PATCH_TITLE_ID (update/DLC title id for addon linking)");
    }
    const auto base = require("BASE");
    if (!base) {
        return fail("missing BASE (base game declaration; no file check)");
    }

    M3mPlaylist out;
    out.title_id = *title_id;
    out.rom = *rom;
    out.patch_title_id = *patch_title_id;
    out.base = *base;
    out.rom_path = (m3m_path.parent_path() / out.rom).lexically_normal();

    std::error_code ec;
    if (!std::filesystem::is_regular_file(out.rom_path, ec) || ec) {
        return fail("ROM file not found: " + out.rom);
    }
    return out;
}

std::optional<std::string> parse_m3m_rom_basename(const std::filesystem::path& m3m_path) {
    std::ifstream in(m3m_path);
    if (!in) {
        return std::nullopt;
    }
    std::string line;
    while (std::getline(in, line)) {
        std::string_view view = line;
        if (!view.empty() && view.back() == '\r') {
            view.remove_suffix(1);
        }
        if (is_comment_or_blank(view)) {
            continue;
        }
        view = trim_view(view);
        const auto eq = view.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        auto key = std::string(trim_view(view.substr(0, eq)));
        for (char& ch : key) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        if (key != "ROM") {
            continue;
        }
        const auto value = std::string(trim_view(view.substr(eq + 1)));
        if (value.empty()) {
            return std::nullopt;
        }
        return std::filesystem::path(value).filename().string();
    }
    return std::nullopt;
}

} // namespace archstreamer
