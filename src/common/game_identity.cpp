#include "common/game_identity.hpp"

#include <cctype>
#include <utility>

namespace archstreamer {
namespace {

std::string_view trim_view(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return value;
}

bool take_prefixed_line(std::string_view& rest, std::string_view prefix, std::string& out) {
    if (rest.size() < prefix.size() || rest.substr(0, prefix.size()) != prefix) {
        return false;
    }
    rest.remove_prefix(prefix.size());
    const auto nl = rest.find('\n');
    std::string_view line = nl == std::string_view::npos ? rest : rest.substr(0, nl);
    out = std::string(trim_view(line));
    if (nl == std::string_view::npos) {
        rest = {};
    } else {
        rest.remove_prefix(nl + 1);
    }
    return true;
}

} // namespace

std::string identity_key_for(
    std::string_view system_key,
    std::string_view canonical_name,
    std::string_view version,
    std::string_view language,
    std::string_view region) {
    return
        "system=" + std::string(system_key) +
        "\nname=" + std::string(canonical_name) +
        "\nversion=" + std::string(version) +
        "\nlanguage=" + std::string(language) +
        "\nregion=" + std::string(region);
}

std::optional<GameIdentityParts> parse_identity_key(std::string_view identity_key) {
    GameIdentityParts parts;
    std::string_view rest = identity_key;
    // Accept optional trailing newline.
    if (!rest.empty() && rest.back() == '\n') {
        rest.remove_suffix(1);
    }
    if (!take_prefixed_line(rest, "system=", parts.system_key)
        || !take_prefixed_line(rest, "name=", parts.canonical_name)
        || !take_prefixed_line(rest, "version=", parts.version)
        || !take_prefixed_line(rest, "language=", parts.language)
        || !take_prefixed_line(rest, "region=", parts.region)
        || !rest.empty()) {
        return std::nullopt;
    }
    return parts;
}

GameIdentity GameIdentity::from_parts(GameIdentityParts parts) {
    GameIdentity identity;
    identity.parts = std::move(parts);
    identity.identity_key = identity_key_for(
        identity.parts.system_key,
        identity.parts.canonical_name,
        identity.parts.version,
        identity.parts.language,
        identity.parts.region);
    identity.game_id = sha256_hex(identity.identity_key);
    return identity;
}

GameIdentity GameIdentity::from_parts(
    std::string_view system_key,
    std::string_view canonical_name,
    std::string_view version,
    std::string_view language,
    std::string_view region) {
    return from_parts(GameIdentityParts{
        std::string(system_key),
        std::string(canonical_name),
        std::string(version),
        std::string(language),
        std::string(region),
    });
}

bool GameIdentity::identity_matches_parts() const {
    return identity_key == identity_key_for(
               parts.system_key,
               parts.canonical_name,
               parts.version,
               parts.language,
               parts.region);
}

bool GameIdentity::game_id_matches_identity() const {
    if (game_id.empty() || identity_key.empty()) {
        return false;
    }
    return game_id == sha256_hex(identity_key);
}

bool catalog_version_is_unspecified(std::string_view version) {
    return version.empty() || version == "unknown";
}

bool catalog_version_is_base(std::string_view version) {
    return catalog_version_is_unspecified(version) || version == "0" || version == "1";
}

bool catalog_version_contains_rev(std::string_view version) {
    std::string lower(version);
    for (char& ch : lower) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return lower.find("rev") != std::string::npos;
}

bool catalog_version_is_unlabeled(std::string_view version) {
    return catalog_version_is_base(version) || catalog_version_contains_rev(version);
}

std::string catalog_version_normalize(std::string_view version) {
    if (catalog_version_is_unspecified(version)) {
        return "0";
    }
    return std::string(version);
}

bool same_catalog_version(std::string_view left, std::string_view right) {
    return catalog_version_normalize(left) == catalog_version_normalize(right);
}

std::string catalog_version_display_token(std::string_view version) {
    if (catalog_version_is_unlabeled(version)) {
        return {};
    }
    std::string ver(version);
    // Numeric builds: 1-3-2 → 1.3.2
    bool numeric = !ver.empty();
    for (char ch : ver) {
        if (!(std::isdigit(static_cast<unsigned char>(ch)) || ch == '-')) {
            numeric = false;
            break;
        }
    }
    if (numeric) {
        if (ver.find('-') != std::string::npos) {
            for (char& ch : ver) {
                if (ch == '-') {
                    ch = '.';
                }
            }
        }
        return ver;
    }

    // Text tags (special-edition): Title Case words, dashes/underscores → spaces.
    for (char& ch : ver) {
        if (ch == '-' || ch == '_') {
            ch = ' ';
        }
    }
    std::string out;
    out.reserve(ver.size());
    bool new_word = true;
    for (char ch : ver) {
        if (ch == ' ' || ch == '\t') {
            if (!out.empty() && out.back() != ' ') {
                out.push_back(' ');
            }
            new_word = true;
            continue;
        }
        const auto u = static_cast<unsigned char>(ch);
        if (new_word) {
            out.push_back(static_cast<char>(std::toupper(u)));
            new_word = false;
        } else {
            out.push_back(static_cast<char>(std::tolower(u)));
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

std::string catalog_label_for(std::string_view display_name, std::string_view version) {
    const auto shown = catalog_version_display_token(version);
    if (shown.empty()) {
        return std::string(display_name);
    }
    return std::string(display_name) + " (" + shown + ")";
}

std::string catalog_rom_stem_for(std::string_view display_name, std::string_view version) {
    return catalog_label_for(display_name, version);
}

std::string catalog_content_stem_for(std::string_view display_name, std::string_view version) {
    std::string stem = catalog_rom_stem_for(display_name, version);
    for (char& ch : stem) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return stem;
}

std::string catalog_save_stem_for(std::string_view display_name, std::string_view version) {
    return catalog_rom_stem_for(display_name, version);
}

bool catalog_save_stem_matches(
    std::string_view disk_stem,
    std::string_view display_name,
    std::string_view version) {
    if (disk_stem.empty() || display_name.empty()) {
        return false;
    }
    const auto want = catalog_save_stem_for(display_name, version);
    if (disk_stem == want) {
        return true;
    }
    const auto want_l = catalog_content_stem_for(display_name, version);
    if (want_l.empty()) {
        return false;
    }
    if (disk_stem.size() != want_l.size()) {
        return false;
    }
    for (std::size_t i = 0; i < disk_stem.size(); ++i) {
        const auto a = static_cast<unsigned char>(disk_stem[i]);
        const auto b = static_cast<unsigned char>(want_l[i]);
        if (std::tolower(a) != b) {
            return false;
        }
    }
    return true;
}

std::string catalog_normalize_save_stem_intent(std::string_view disk_stem) {
    std::string out{disk_stem};
    if (out.size() < 3) {
        return out;
    }
    // "Title v1.1" / "Title V2" → "Title (1.1)" / "Title (2)" (intent match only).
    std::size_t i = out.size();
    while (i > 0) {
        const auto ch = static_cast<unsigned char>(out[i - 1]);
        if (std::isdigit(ch) || out[i - 1] == '.') {
            --i;
            continue;
        }
        break;
    }
    if (i >= out.size() || i < 2) {
        return out;
    }
    const auto ver = out.substr(i);
    if (ver.empty() || ver.front() == '.' || ver.back() == '.') {
        return out;
    }
    bool saw_digit = false;
    for (char ch : ver) {
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            saw_digit = true;
        } else if (ch != '.') {
            return out;
        }
    }
    if (!saw_digit) {
        return out;
    }
    std::size_t vpos = i;
    while (vpos > 0 && (out[vpos - 1] == ' ' || out[vpos - 1] == '\t')) {
        --vpos;
    }
    if (vpos == 0) {
        return out;
    }
    const char marker = out[vpos - 1];
    if (marker != 'v' && marker != 'V') {
        return out;
    }
    std::size_t title_end = vpos - 1;
    while (title_end > 0 && (out[title_end - 1] == ' ' || out[title_end - 1] == '\t')) {
        --title_end;
    }
    if (title_end == 0) {
        return out;
    }
    return out.substr(0, title_end) + " (" + ver + ")";
}

void strip_matching_version_label(std::string& display_name, std::string_view version) {
    if (display_name.empty()) {
        return;
    }

    auto equals_ci = [](std::string_view a, std::string_view b) {
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
    };

    auto try_strip = [&](std::string_view suffix) {
        if (suffix.empty() || display_name.size() < suffix.size()) {
            return false;
        }
        const auto tail = std::string_view(display_name).substr(display_name.size() - suffix.size());
        if (!equals_ci(tail, suffix)) {
            return false;
        }
        display_name.resize(display_name.size() - suffix.size());
        while (!display_name.empty() && (display_name.back() == ' ' || display_name.back() == '\t')) {
            display_name.pop_back();
        }
        return true;
    };

    // Bookkeeping unlabeled flags must not linger in the title.
    (void)(try_strip(" (0)") || try_strip(" (1)") || try_strip(" (unknown)"));

    if (catalog_version_is_unlabeled(version)) {
        return;
    }

    // Prefer the human edge form ("Special Edition", "1.3.2"), then raw/dashed/dotted.
    if (const auto shown = catalog_version_display_token(version); !shown.empty()) {
        if (try_strip(" (" + shown + ")")) {
            return;
        }
    }

    std::string dotted(version);
    std::string dashed(version);
    for (char& ch : dotted) {
        if (ch == '-') {
            ch = '.';
        }
    }
    for (char& ch : dashed) {
        if (ch == '.') {
            ch = '-';
        }
    }

    if (try_strip(" (" + dotted + ")") || try_strip(" (" + dashed + ")")
        || try_strip(" (" + std::string(version) + ")")) {
        return;
    }
}

std::string asset_key_for(
    std::string_view system_key,
    std::string_view canonical_name,
    std::string_view language,
    std::string_view region,
    std::string_view version) {
    return
        std::string(system_key) + "/" +
        std::string(canonical_name) + "/" +
        std::string(language) + "/" +
        std::string(region) + "/" +
        std::string(version);
}

CatalogIdentityFields compose_catalog_identity(
    std::string system_key,
    std::string canonical_name,
    std::string version,
    std::string language,
    std::string region) {
    CatalogIdentityFields out;
    out.system_key = std::move(system_key);
    out.canonical_name = std::move(canonical_name);
    out.version = catalog_version_normalize(version);
    out.language = language.empty() ? "en" : std::move(language);
    out.region = region.empty() ? "unknown" : std::move(region);
    const auto identity = GameIdentity::from_parts(
        out.system_key,
        out.canonical_name,
        out.version,
        out.language,
        out.region);
    out.identity_key = identity.identity_key;
    out.game_id = identity.game_id;
    out.asset_key = asset_key_for(
        out.system_key,
        out.canonical_name,
        out.language,
        out.region,
        out.version);
    return out;
}

} // namespace archstreamer
