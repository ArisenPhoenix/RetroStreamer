#include "common/catalog_paths.hpp"
#include "common/dlc_paths.hpp"
#include "host/catalog_repair.hpp"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

void print_usage() {
    std::cout
        << "catalog_repair — re-apply intended game_meta identities from the edit log\n\n"
        << "Usage: catalog_repair [options]\n"
        << "  --dry-run          Show the plan without changing anything\n"
        << "  --align-all        Also rename ROM/Meta files for untouched rows\n"
        << "  --no-filesystem    DB only (skip save/art/DLC/ROM renames)\n"
        << "  --rom-root <path>  ROM root (default: inferred from content paths)\n"
        << "  --art-root <path>  Art root (default: <ROMS>/Art)\n"
        << "  --save-root <path> Save root (default ~/.local/share/archstreamer/saves)\n";
}

std::filesystem::path default_save_root() {
    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        return {};
    }
    return std::filesystem::path{home} / ".local" / "share" / "archstreamer" / "saves";
}

/** Find the ROMS/Games ancestor shared by catalogued content paths. */
std::filesystem::path infer_rom_root(const archstreamer::GameMetaStore& store) {
    for (const auto& row : store.list_games()) {
        if (row.content_path.empty()) {
            continue;
        }
        for (auto dir = std::filesystem::path{row.content_path}.parent_path();
             !dir.empty() && dir != dir.root_path();
             dir = dir.parent_path()) {
            if (dir.filename() == "Games" && dir.parent_path().filename() == "ROMS") {
                return dir;
            }
        }
    }
    return {};
}

} // namespace

int main(int argc, char** argv) {
    archstreamer::CatalogRepairOptions options;
    options.fs.save_root = default_save_root();

    for (int i = 1; i < argc; ++i) {
        const auto arg = std::string{argv[i]};
        auto next_value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << '\n';
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--dry-run") {
            options.dry_run = true;
        } else if (arg == "--align-all") {
            options.align_all_rows = true;
        } else if (arg == "--no-filesystem") {
            options.fs.apply_filesystem = false;
        } else if (arg == "--rom-root") {
            options.fs.rom_root = next_value("--rom-root");
        } else if (arg == "--art-root") {
            options.fs.art_root = next_value("--art-root");
        } else if (arg == "--save-root") {
            options.fs.save_root = next_value("--save-root");
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << "\n\n";
            print_usage();
            return 2;
        }
    }
    archstreamer::GameMetaStore store;
    archstreamer::GameMetaEditLog edits;

    if (options.fs.rom_root.empty()) {
        options.fs.rom_root = infer_rom_root(store);
    }
    if (options.fs.art_root.empty() && !options.fs.rom_root.empty()) {
        options.fs.art_root = options.fs.rom_root.parent_path() / "Art";
    }
    if (options.fs.dlc_root.empty() && !options.fs.rom_root.empty()) {
        options.fs.dlc_root = archstreamer::default_dlc_root_for(options.fs.rom_root);
    }

    std::cout << "meta DB:  " << store.path() << "\nedits DB: " << edits.path()
              << "\nROM root: " << options.fs.rom_root
              << "\nArt root: " << options.fs.art_root
              << "\nSaves:    " << options.fs.save_root << "\n\n";

    const auto report = archstreamer::repair_catalog_from_edits(store, edits, options);
    for (const auto& line : report.lines) {
        std::cout << line << '\n';
    }
    std::cout << "\nchains=" << report.chains << " repaired=" << report.repaired
              << " already_correct=" << report.already_correct
              << " folded=" << report.folded_duplicates
              << " renamed_files=" << report.aligned_files
              << " missing=" << report.missing_rows << '\n';
    if (options.dry_run) {
        std::cout << "(dry run — nothing was changed)\n";
    }
    return 0;
}
