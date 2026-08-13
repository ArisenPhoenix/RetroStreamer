#include "paths_panel.hpp"

#include "common/catalog_paths.hpp"
#include "common/platform/paths.hpp"
#include "host/console/game_catalog_scanner.hpp"
#include "host/console/libretro_core_registry.hpp"
#include "host/console/save_profile.hpp"

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QWidget>

#include <cstdlib>
#include <utility>

namespace archstreamer::gui {

bool host_runtime_available() {
    return true;
}

PathRootDefaults default_path_roots() {
    PathRootDefaults defaults;
    defaults.art_root = QString::fromUtf8(DefaultArtRoot);
    defaults.rom_root = QString::fromUtf8(DefaultRomRoot);
    defaults.meta_root = QString::fromUtf8(DefaultMetaRoot);
    defaults.save_root = QString::fromStdString(default_save_profile_root().string());
    if (const char* configured = std::getenv("ARCHSTREAMER_HOST_CONFIG");
        configured != nullptr && configured[0] != '\0') {
        defaults.host_config = QString::fromUtf8(configured);
    } else {
        defaults.host_config =
            QSettings(QStringLiteral("ArchStreamer"), QStringLiteral("ArchStreamer")).fileName();
    }
    defaults.log_root = QString::fromStdString(archstreamer_cache_directory());
    return defaults;
}

void create_host_path_rows(PathsPanel& panel, QWidget* parent) {
    panel.rom_root = new QLineEdit(parent);
    panel.rom_root->setPlaceholderText(QStringLiteral("…/ROMS/Games  (under your Gaming root)"));

    panel.meta_root = new QLineEdit(parent);
    panel.meta_root->setPlaceholderText(QStringLiteral("…/ROMS/Meta  (under your Gaming root)"));

    panel.save_root = new QLineEdit(default_path_roots().save_root, parent);
    panel.save_root->setToolTip(
        "Directory where client usernames store saves, states, and emulator profiles.\n"
        "Layout: <save-root>/<username>/…\n"
        "Flatpak: path must be visible to this app (home is allowed; other disks need\n"
        "flatpak override --filesystem=<path>:rw). Host sessions use the native host_runner\n"
        "path on the host OS.");
    panel.save_root_browse = new QPushButton("Browse…", parent);
    panel.save_root_create = new QPushButton("Create", parent);
    panel.save_root_create->setToolTip(
        "Create this directory (and parents) if it is missing and writable.");
    panel.save_root_status = new QLabel(parent);
    panel.save_root_status->setWordWrap(true);
    panel.save_root_status->setStyleSheet(QStringLiteral("color: #a33;"));

    panel.host_config = new QLineEdit(default_path_roots().host_config, parent);
    panel.host_config->setToolTip(
        "Default config passed to host_runner --config for remote starts.\n"
        "By default this reuses the GUI settings file shown above.");

    panel.log_root = new QLineEdit(default_path_roots().log_root, parent);
    panel.log_root->setToolTip(
        "Directory where host_runner writes durable host_<control-port>.log files.\n"
        "Leave blank to use the default ArchStreamer cache directory.");
    panel.log_root_browse = new QPushButton("Browse…", parent);
    panel.log_root_create = new QPushButton("Create", parent);
    panel.log_root_create->setToolTip("Create this log directory if it is missing.");

    panel.native_host_runner = new QLineEdit(parent);
    panel.native_host_runner->setPlaceholderText(
        "auto (ARCHSTREAMER_HOST_RUNNER or common paths)");
    panel.native_host_runner->setToolTip(
        "When running as a Flatpak, Host start uses flatpak-spawn --host on this binary.\n"
        "Point it at a native host_runner built outside the sandbox (gamecope/uinput/Switch).");
}

std::vector<GameArtImportTarget> scan_art_import_targets(
    const std::filesystem::path& rom_root,
    const std::filesystem::path& meta_root) {
    const auto catalog = scan_game_catalog(
        rom_root,
        LibretroCoreRegistry::ubuntu_defaults(),
        meta_root);
    const auto list = catalog.list();
    std::vector<GameArtImportTarget> targets;
    targets.reserve(list.games.size());
    for (const auto& game : list.games) {
        GameArtImportTarget target;
        target.asset_key = game.asset_key;
        target.display_name = game.display_name;
        target.canonical_name = game.canonical_name;
        if (const auto hosted = catalog.find_hosted(game.id); hosted.has_value()) {
            target.content_path = hosted->get().content_path;
        }
        targets.push_back(std::move(target));
    }
    return targets;
}

} // namespace archstreamer::gui
