#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

enum class GameAssetKind {
    Grid,
    Hero,
    Logo,
    Icon,
    Boxart,
    Screenshot,
};

struct GameAssets {
    std::optional<std::filesystem::path> grid;
    std::optional<std::filesystem::path> hero;
    std::optional<std::filesystem::path> logo;
    std::optional<std::filesystem::path> icon;
    std::optional<std::filesystem::path> boxart;
    std::optional<std::filesystem::path> screenshot;
};

std::filesystem::path default_assets_root_for(const std::filesystem::path& content_root);
std::filesystem::path asset_directory_for(
    const std::filesystem::path& content_root,
    const std::filesystem::path& assets_root,
    const std::filesystem::path& content_path);
std::string_view asset_base_name(GameAssetKind kind);
std::vector<std::string_view> asset_base_names(GameAssetKind kind);
std::optional<std::filesystem::path> find_asset_file(
    const std::filesystem::path& asset_directory,
    GameAssetKind kind);

class LocalGameAssetProvider {
public:
    LocalGameAssetProvider(std::filesystem::path content_root, std::filesystem::path assets_root = {});

    const std::filesystem::path& content_root() const;
    const std::filesystem::path& assets_root() const;

    std::filesystem::path directory_for(const std::filesystem::path& content_path) const;
    std::filesystem::path directory_for_asset_key(std::string_view asset_key) const;
    GameAssets assets_for(const std::filesystem::path& content_path) const;
    GameAssets assets_for_asset_key(std::string_view asset_key) const;

private:
    static GameAssets assets_in_directory(const std::filesystem::path& directory);

    std::filesystem::path content_root_;
    std::filesystem::path assets_root_;
};

} // namespace archstreamer
