#include "common/art_transfer.hpp"

#include "common/game_assets.hpp"
#include "common/platform/paths.hpp"
#include "common/sha256.hpp"

#include <fstream>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

namespace archstreamer {
namespace {

std::optional<GameAssetKind> kind_for_role(std::string_view role) {
    if (role == "boxart") {
        return GameAssetKind::Boxart;
    }
    if (role == "grid") {
        return GameAssetKind::Grid;
    }
    if (role == "icon") {
        return GameAssetKind::Icon;
    }
    if (role == "screenshot") {
        return GameAssetKind::Screenshot;
    }
    if (role == "logo") {
        return GameAssetKind::Logo;
    }
    if (role == "hero") {
        return GameAssetKind::Hero;
    }
    return std::nullopt;
}

std::string sanitize_path_component(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char character : value) {
        if ((character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '.' || character == '-' || character == '_') {
            out.push_back(character);
        } else {
            out.push_back('_');
        }
    }
    if (out.empty()) {
        out = "host";
    }
    return out;
}

std::string sha256_of_bytes(std::span<const std::uint8_t> bytes) {
    const auto view = std::string_view(
        reinterpret_cast<const char*>(bytes.data()),
        bytes.size());
    return sha256_hex(view);
}

std::optional<std::vector<std::uint8_t>> read_file_capped(
    const std::filesystem::path& path,
    std::size_t max_size) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    file.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(file.tellg());
    if (size == 0 || size > max_size) {
        return std::nullopt;
    }
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    if (!file) {
        return std::nullopt;
    }
    return data;
}

} // namespace

ArtAssetResponse load_art_asset_response(
    const std::filesystem::path& art_root,
    std::string_view asset_key,
    std::string_view role,
    std::string_view cached_sha256) {
    ArtAssetResponse response;
    response.asset_key = std::string(asset_key);
    response.role = std::string(role);

    const auto kind = kind_for_role(role);
    if (!kind.has_value() || asset_key.empty()) {
        return response;
    }

    LocalGameAssetProvider provider({}, art_root);
    const auto directory = provider.directory_for_asset_key(asset_key);
    const auto path = find_asset_file(directory, *kind);
    if (!path.has_value()) {
        return response;
    }

    auto data = read_file_capped(*path, 16 * 1024 * 1024);
    if (!data.has_value()) {
        return response;
    }

    response.found = true;
    response.extension = path->extension().string();
    response.content_sha256 = sha256_of_bytes(*data);
    if (!cached_sha256.empty() && cached_sha256 == response.content_sha256) {
        // Client already has this exact blob — omit body.
        return response;
    }
    response.data = std::move(*data);
    return response;
}

bool art_asset_exists_locally(
    const std::filesystem::path& art_root,
    std::string_view asset_key,
    std::string_view role) {
    const auto kind = kind_for_role(role);
    if (!kind.has_value() || asset_key.empty()) {
        return false;
    }
    LocalGameAssetProvider provider({}, art_root);
    return find_asset_file(provider.directory_for_asset_key(asset_key), *kind).has_value();
}

std::string local_art_asset_sha256(
    const std::filesystem::path& art_root,
    std::string_view asset_key,
    std::string_view role) {
    const auto kind = kind_for_role(role);
    if (!kind.has_value() || asset_key.empty()) {
        return {};
    }
    LocalGameAssetProvider provider({}, art_root);
    const auto path = find_asset_file(provider.directory_for_asset_key(asset_key), *kind);
    if (!path.has_value()) {
        return {};
    }
    auto data = read_file_capped(*path, 16 * 1024 * 1024);
    if (!data.has_value()) {
        return {};
    }
    return sha256_of_bytes(*data);
}

bool write_art_asset_to_cache(
    const std::filesystem::path& cache_art_root,
    const ArtAssetResponse& response) {
    if (!response.found || response.asset_key.empty() || response.role.empty()) {
        return false;
    }
    // Not-modified: host confirmed hash match with empty body.
    if (response.data.empty()) {
        return !response.content_sha256.empty();
    }

    auto extension = response.extension;
    if (extension.empty()) {
        extension = ".png";
    }
    if (extension.front() != '.') {
        extension.insert(extension.begin(), '.');
    }

    const auto directory = cache_art_root / response.asset_key;
    std::filesystem::create_directories(directory);
    const auto path = directory / (response.role + extension);

    // Drop any other extension for this role so resolve picks the new file.
    if (const auto kind = kind_for_role(response.role); kind.has_value()) {
        LocalGameAssetProvider provider({}, cache_art_root);
        if (const auto existing =
                find_asset_file(provider.directory_for_asset_key(response.asset_key), *kind);
            existing.has_value() && *existing != path) {
            std::error_code ec;
            std::filesystem::remove(*existing, ec);
        }
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(
        reinterpret_cast<const char*>(response.data.data()),
        static_cast<std::streamsize>(response.data.size()));
    return static_cast<bool>(file);
}

std::filesystem::path host_art_cache_root(std::string_view host_id) {
    std::filesystem::path base;
    if (const auto cache = archstreamer_cache_directory(); !cache.empty()) {
        base = std::filesystem::path(cache) / "hosts";
    } else {
        base = std::filesystem::current_path() / "archstreamer-hosts";
    }
    return base / sanitize_path_component(host_id) / "Art";
}

std::string sanitize_host_cache_id(std::string_view username, std::string_view address) {
    return sanitize_path_component(username) + "@" + sanitize_path_component(address);
}

} // namespace archstreamer
