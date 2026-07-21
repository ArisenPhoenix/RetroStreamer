#include "common/game_assets.hpp"
#include "host/game_catalog_scanner.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

namespace {

void print_asset(const char* label, const std::optional<std::filesystem::path>& path) {
    if (path.has_value()) {
        std::cout << "\n    " << label << '=' << *path;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: asset_probe <content-root> [metadata-root] [assets-root] [--create-dirs]\n";
        return 2;
    }

    const auto content_root = std::filesystem::path{argv[1]};
    auto metadata_root = std::filesystem::path{};
    auto assets_root = std::filesystem::path{};
    auto create_dirs = false;
    for (int i = 2; i < argc; ++i) {
        const auto arg = std::string{argv[i]};
        if (arg == "--create-dirs") {
            create_dirs = true;
        } else if (metadata_root.empty()) {
            metadata_root = arg;
        } else if (assets_root.empty()) {
            assets_root = arg;
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return 2;
        }
    }

    const auto catalog = archstreamer::scan_game_catalog(
        content_root,
        archstreamer::LibretroCoreRegistry::ubuntu_defaults(),
        metadata_root);
    const auto asset_provider = archstreamer::LocalGameAssetProvider{content_root, assets_root};
    const auto list = catalog.list();

    std::cout
        << "Assets root: " << asset_provider.assets_root()
        << "\nFound " << list.games.size() << " supported games.\n";

    for (const auto& game : list.games) {
        const auto directory = asset_provider.directory_for_asset_key(game.asset_key);
        const auto assets = asset_provider.assets_for_asset_key(game.asset_key);
        if (create_dirs) {
            std::filesystem::create_directories(directory);
        }

        std::cout
            << game.display_name
            << "\n  id=" << game.id
            << "\n  canonical_name=" << game.canonical_name
            << "\n  asset_key=" << game.asset_key
            << "\n  assets=" << directory;
        print_asset("grid", assets.grid);
        print_asset("hero", assets.hero);
        print_asset("logo", assets.logo);
        print_asset("icon", assets.icon);
        print_asset("boxart", assets.boxart);
        print_asset("screenshot", assets.screenshot);
        std::cout << '\n';
    }
}
