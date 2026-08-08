#include "paths_panel.hpp"

#include "common/catalog_paths.hpp"

namespace archstreamer::gui {

bool host_runtime_available() {
    return false;
}

PathRootDefaults default_path_roots() {
    // Art is the only root a client resolves locally; the rest belong to a host.
    PathRootDefaults defaults;
    defaults.art_root = QString::fromUtf8(DefaultArtRoot);
    return defaults;
}

void create_host_path_rows(PathsPanel&, QWidget*) {}

std::vector<GameArtImportTarget> scan_art_import_targets(
    const std::filesystem::path&,
    const std::filesystem::path&) {
    return {};
}

} // namespace archstreamer::gui
