#include "common/game_assets.hpp"

#include <array>
#include <utility>

namespace archstreamer {

std::filesystem::path default_assets_root_for(const std::filesystem::path& content_root) {
    return content_root.parent_path() / "Art";
}

std::filesystem::path default_placeholder_art_path(const std::filesystem::path& art_root) {
    return art_root / "default" / "default_image.png";
}

std::filesystem::path asset_directory_for(
    const std::filesystem::path& content_root,
    const std::filesystem::path& assets_root,
    const std::filesystem::path& content_path) {
    auto relative = std::filesystem::relative(content_path, content_root);
    relative.replace_extension();
    return assets_root / relative;
}

std::string_view asset_base_name(GameAssetKind kind) {
    switch (kind) {
        case GameAssetKind::Grid:
            return "grid";
        case GameAssetKind::Hero:
            return "hero";
        case GameAssetKind::Logo:
            return "logo";
        case GameAssetKind::Icon:
            return "icon";
        case GameAssetKind::Boxart:
            return "boxart";
        case GameAssetKind::Screenshot:
            return "screenshot";
    }

    return "";
}

std::vector<std::string_view> asset_base_names(GameAssetKind kind) {
    switch (kind) {
        case GameAssetKind::Grid:
            return {"grid", "portrait", "capsule"};
        case GameAssetKind::Hero:
            return {"hero", "wide", "background"};
        case GameAssetKind::Logo:
            return {"logo"};
        case GameAssetKind::Icon:
            return {"icon"};
        case GameAssetKind::Boxart:
            return {"boxart", "cover"};
        case GameAssetKind::Screenshot:
            return {"screenshot", "screen"};
    }

    return {};
}

std::optional<std::filesystem::path> find_asset_file(
    const std::filesystem::path& asset_directory,
    GameAssetKind kind) {
    static constexpr auto extensions = std::array<std::string_view, 5>{".png", ".jpg", ".jpeg", ".webp", ".ico"};
    for (const auto base_name : asset_base_names(kind)) {
        for (const auto extension : extensions) {
            auto path = asset_directory / std::string(base_name);
            path += extension;
            if (std::filesystem::is_regular_file(path)) {
                return path;
            }
        }
    }

    return std::nullopt;
}

LocalGameAssetProvider::LocalGameAssetProvider(std::filesystem::path content_root, std::filesystem::path assets_root)
    : content_root_(std::move(content_root)),
      assets_root_(assets_root.empty() ? default_assets_root_for(content_root_) : std::move(assets_root)) {
}

const std::filesystem::path& LocalGameAssetProvider::content_root() const {
    return content_root_;
}

const std::filesystem::path& LocalGameAssetProvider::assets_root() const {
    return assets_root_;
}

std::filesystem::path LocalGameAssetProvider::directory_for(const std::filesystem::path& content_path) const {
    return asset_directory_for(content_root_, assets_root_, content_path);
}

std::filesystem::path LocalGameAssetProvider::directory_for_asset_key(std::string_view asset_key) const {
    return assets_root_ / std::filesystem::path{std::string(asset_key)};
}

GameAssets LocalGameAssetProvider::assets_for(const std::filesystem::path& content_path) const {
    const auto directory = directory_for(content_path);
    return assets_in_directory(directory);
}

GameAssets LocalGameAssetProvider::assets_for_asset_key(std::string_view asset_key) const {
    return assets_in_directory(directory_for_asset_key(asset_key));
}

GameAssets LocalGameAssetProvider::assets_in_directory(const std::filesystem::path& directory) {
    return GameAssets{
        find_asset_file(directory, GameAssetKind::Grid),
        find_asset_file(directory, GameAssetKind::Hero),
        find_asset_file(directory, GameAssetKind::Logo),
        find_asset_file(directory, GameAssetKind::Icon),
        find_asset_file(directory, GameAssetKind::Boxart),
        find_asset_file(directory, GameAssetKind::Screenshot),
    };
}

std::filesystem::path resolve_game_display_art(
    const LocalGameAssetProvider& provider,
    std::string_view asset_key) {
    return resolve_game_display_art(provider, asset_key, {}, {});
}

namespace {

std::optional<std::filesystem::path> find_titled_srm_art(
    const std::filesystem::path& art_root,
    std::string_view title) {
    if (title.empty()) {
        return std::nullopt;
    }

    static constexpr auto folders = std::array<std::string_view, 5>{
        "poster",
        "boxart",
        "grids",
        "heroes",
        "icons",
    };
    static constexpr auto extensions = std::array<std::string_view, 4>{
        ".png",
        ".jpg",
        ".jpeg",
        ".webp",
    };

    for (const auto folder : folders) {
        for (const auto extension : extensions) {
            auto path = art_root / std::string(folder) / std::string(title);
            path += extension;
            if (std::filesystem::is_regular_file(path)) {
                return path;
            }
        }
    }
    return std::nullopt;
}

std::string canonical_to_title(std::string_view canonical_name) {
    std::string title;
    title.reserve(canonical_name.size());
    bool capitalize = true;
    for (const char character : canonical_name) {
        if (character == '-' || character == '_') {
            title.push_back(' ');
            capitalize = true;
            continue;
        }
        if (capitalize && character >= 'a' && character <= 'z') {
            title.push_back(static_cast<char>(character - 'a' + 'A'));
        } else {
            title.push_back(character);
        }
        capitalize = false;
    }
    return title;
}

} // namespace

std::filesystem::path resolve_game_display_art(
    const LocalGameAssetProvider& provider,
    std::string_view asset_key,
    std::string_view display_name,
    std::string_view canonical_name) {
    const auto assets = provider.assets_for_asset_key(asset_key);
    if (assets.boxart.has_value()) {
        return *assets.boxart;
    }
    if (assets.grid.has_value()) {
        return *assets.grid;
    }
    if (assets.icon.has_value()) {
        return *assets.icon;
    }
    if (assets.screenshot.has_value()) {
        return *assets.screenshot;
    }

    if (const auto titled = find_titled_srm_art(provider.assets_root(), display_name); titled.has_value()) {
        return *titled;
    }
    if (!canonical_name.empty()) {
        if (const auto titled = find_titled_srm_art(provider.assets_root(), canonical_to_title(canonical_name));
            titled.has_value()) {
            return *titled;
        }
    }

    return default_placeholder_art_path(provider.assets_root());
}

} // namespace archstreamer
