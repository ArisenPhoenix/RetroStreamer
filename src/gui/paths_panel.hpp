#pragma once

#include "common/steam_art_import.hpp"

#include <QString>

#include <filesystem>
#include <vector>

class QLabel;
class QLineEdit;
class QPushButton;
class QWidget;

namespace archstreamer::gui {

/**
 * True when this build links the host runtime. ROM / Meta / Game Saves roots and
 * the native host_runner override only exist then; ask this (or null-check the
 * row) rather than testing ARCHSTREAMER_HAS_HOST at the use site.
 */
bool host_runtime_available();

/**
 * Every path row on the Paths tab. Host-only rows stay null in a client-only
 * build, so readers null-check a pointer instead of guarding on the build macro.
 */
struct PathsPanel {
    QLineEdit* art_root = nullptr;
    QLineEdit* rom_root = nullptr;
    QLineEdit* meta_root = nullptr;
    QLineEdit* save_root = nullptr;
    QPushButton* save_root_browse = nullptr;
    QPushButton* save_root_create = nullptr;
    QLabel* save_root_status = nullptr;
    QLineEdit* native_host_runner = nullptr;
};

/** Create the rows only a host build has. No-op otherwise. */
void create_host_path_rows(PathsPanel& panel, QWidget* parent);

/** Root shown when the user has never set one. Host-only entries stay empty otherwise. */
struct PathRootDefaults {
    QString art_root;
    QString rom_root;
    QString meta_root;
    QString save_root;
};

PathRootDefaults default_path_roots();

/**
 * Steam-art import targets scanned from a local ROM catalog. Empty in a
 * client-only build, which ships no catalog scanner.
 */
std::vector<GameArtImportTarget> scan_art_import_targets(
    const std::filesystem::path& rom_root,
    const std::filesystem::path& meta_root);

/** Trimmed text of an optional row; empty when the row does not exist. */
QString path_field_text(const QLineEdit* field);

/** Expand a leading `~` against the user's home directory. */
QString expand_user_path(QString path);

/** Trimmed, `~`-expanded row text as a path; empty when blank or absent. */
std::filesystem::path path_field_value(const QLineEdit* field);

} // namespace archstreamer::gui
