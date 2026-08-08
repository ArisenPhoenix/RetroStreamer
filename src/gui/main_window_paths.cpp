#include "main_window.hpp"

#include "game_picker_widget.hpp"
#include "gui_logging.hpp"
#include "gui_util.hpp"
#include "paths_panel.hpp"
#include "common/dlc_paths.hpp"
#include "common/steam_art_import.hpp"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPixmapCache>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWidget>

#include <exception>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace archstreamer::gui {

namespace {

/** Add a row only when this build has the field; keeps the form free of guards. */
void add_optional_row(QFormLayout* form, const QString& label, QWidget* field) {
    if (field != nullptr) {
        form->addRow(label, field);
    }
}

} // namespace

QWidget* MainWindow::build_paths_tab() {
    auto* page = new QWidget(this);
    auto* root = new QVBoxLayout(page);

    auto* form_box = new QGroupBox("Roots", page);
    auto* form = new QFormLayout(form_box);

    paths_.art_root = new QLineEdit(form_box);
    paths_.art_root->setPlaceholderText(QStringLiteral("…/ROMS/Art  (under your Gaming root)"));
    paths_.art_root->setToolTip(
        "Box art and thumbnails used by the game pickers and the Steam art import.");
    create_host_path_rows(paths_, form_box);

    add_optional_row(form, "Art root", paths_.art_root);
    add_optional_row(form, "ROM root", paths_.rom_root);
    add_optional_row(form, "Meta root", paths_.meta_root);
    if (paths_.save_root != nullptr) {
        auto* save_root_row = new QWidget(form_box);
        auto* save_root_layout = new QHBoxLayout(save_root_row);
        save_root_layout->setContentsMargins(0, 0, 0, 0);
        save_root_layout->addWidget(paths_.save_root, 1);
        save_root_layout->addWidget(paths_.save_root_browse);
        save_root_layout->addWidget(paths_.save_root_create);
        form->addRow("Game Saves Root", save_root_row);
        add_optional_row(form, "", paths_.save_root_status);
    }
    add_optional_row(form, "Native host_runner", paths_.native_host_runner);

    connect_path_fields();
    update_save_root_status();

    root->addWidget(form_box);
    root->addWidget(new QLabel(
        host_runtime_available()
            ? QStringLiteral(
                  "Art / ROM / Meta roots are used by the local host and the Steam art "
                  "import.\n"
                  "Game Saves Root holds one directory per client username: "
                  "<root>/<username>/…\n"
                  "Native host_runner only matters under Flatpak, where Host start "
                  "spawns a host OS build.\n"
                  "Clients cache host art under ~/.cache/archstreamer/hosts/<host>/Art.")
            : QStringLiteral(
                  "Art root is used for local Steam import when available.\n"
                  "Clients cache host art under the ArchStreamer cache directory.\n"
                  "ROM / Meta / Game Saves roots belong to a host-capable build."),
        page));
    root->addStretch();
    return page;
}

void MainWindow::connect_path_fields() {
    if (paths_.art_root != nullptr) {
        connect(paths_.art_root, &QLineEdit::editingFinished, this, [this] {
            apply_art_root_to_pickers();
            persist_settings_if_idle();
        });
        connect(paths_.art_root, &QLineEdit::textChanged, this, [this](const QString&) {
            apply_art_root_to_pickers();
        });
    }
    if (paths_.rom_root != nullptr) {
        connect(paths_.rom_root, &QLineEdit::editingFinished, this, [this] {
            persist_settings_if_idle();
        });
    }
    if (paths_.meta_root != nullptr) {
        connect(paths_.meta_root, &QLineEdit::editingFinished, this, [this] {
            persist_settings_if_idle();
        });
    }
    if (paths_.save_root != nullptr) {
        connect(paths_.save_root, &QLineEdit::editingFinished, this, [this] {
            update_save_root_status();
            // Remember the path only once it names a real directory; keep typing otherwise.
            const auto path = save_root_path();
            if (std::filesystem::is_directory(path)) {
                persist_valid_save_root(path);
            } else {
                persist_settings_if_idle();
            }
        });
        connect(paths_.save_root, &QLineEdit::textChanged, this, [this](const QString&) {
            update_save_root_status();
        });
    }
    if (paths_.save_root_browse != nullptr) {
        connect(paths_.save_root_browse, &QPushButton::clicked, this, [this] {
            browse_save_root();
        });
    }
    if (paths_.save_root_create != nullptr) {
        connect(paths_.save_root_create, &QPushButton::clicked, this, [this] {
            create_save_root();
        });
    }
    if (paths_.native_host_runner != nullptr) {
        connect(paths_.native_host_runner, &QLineEdit::editingFinished, this, [this] {
            persist_settings_if_idle();
        });
    }
}

std::filesystem::path MainWindow::art_root_path() const {
    if (auto path = path_field_value(paths_.art_root); !path.empty()) {
        return path;
    }
    return std::filesystem::path{default_path_roots().art_root.toStdString()};
}

std::filesystem::path MainWindow::rom_root_path() const {
    if (auto path = path_field_value(paths_.rom_root); !path.empty()) {
        return path;
    }
    return std::filesystem::path{default_path_roots().rom_root.toStdString()};
}

std::filesystem::path MainWindow::meta_root_path() const {
    if (auto path = path_field_value(paths_.meta_root); !path.empty()) {
        return path;
    }
    return std::filesystem::path{default_path_roots().meta_root.toStdString()};
}

std::filesystem::path MainWindow::save_root_path() const {
    if (auto path = path_field_value(paths_.save_root); !path.empty()) {
        return path;
    }
    return std::filesystem::path{default_path_roots().save_root.toStdString()};
}

std::filesystem::path MainWindow::dlc_root_path() const {
    if (const auto rom = rom_root_path(); !rom.empty()) {
        return archstreamer::default_dlc_root_for(rom);
    }
    return archstreamer::resolve_dlc_root();
}

QString MainWindow::native_host_runner_override() const {
    return path_field_text(paths_.native_host_runner);
}

void MainWindow::sync_save_root_field_to_path(const std::filesystem::path& path) {
    if (paths_.save_root == nullptr) {
        return;
    }
    const QString text = QString::fromStdString(path.string());
    if (paths_.save_root->text().trimmed() != text) {
        const QSignalBlocker blocker(paths_.save_root);
        paths_.save_root->setText(text);
    }
    update_save_root_status();
}

void MainWindow::persist_valid_save_root(const std::filesystem::path& path) {
    if (!std::filesystem::is_directory(path)) {
        return;
    }
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(path, ec);
    const auto stored = ec ? path : absolute;
    sync_save_root_field_to_path(stored);
    if (restoring_settings_) {
        return;
    }
    QSettings settings(QStringLiteral("ArchStreamer"), QStringLiteral("ArchStreamer"));
    settings.setValue(
        QStringLiteral("host/saveRoot"),
        QString::fromStdString(stored.string()));
}

void MainWindow::update_save_root_status() {
    if (paths_.save_root_status == nullptr || paths_.save_root_create == nullptr) {
        return;
    }
    // Keep the field populated with the live default when blank.
    if (paths_.save_root != nullptr && paths_.save_root->text().trimmed().isEmpty()) {
        const QSignalBlocker blocker(paths_.save_root);
        paths_.save_root->setText(default_path_roots().save_root);
    }
    const auto path = save_root_path();
    const QString qpath = QString::fromStdString(path.string());
    const QFileInfo info(qpath);
    if (info.isDir()) {
        paths_.save_root_status->clear();
        paths_.save_root_create->setEnabled(false);
        return;
    }
    paths_.save_root_create->setEnabled(true);
    QString message = QStringLiteral("Directory does not exist: %1").arg(qpath);
    if (info.exists() && !info.isDir()) {
        message = QStringLiteral("Path exists but is not a directory: %1").arg(qpath);
        paths_.save_root_create->setEnabled(false);
    } else if (running_inside_flatpak()) {
        message += QStringLiteral(
            "\nFlatpak cannot see or create this path unless it is under home "
            "(or granted via: flatpak override --user "
            "--filesystem=<path>:rw io.github.ArisenPhoenix.ArchStreamer). "
            "Create it on the host, or choose a visible directory.");
    } else {
        message += QStringLiteral(" — create it or choose another location.");
    }
    paths_.save_root_status->setText(message);
}

void MainWindow::browse_save_root() {
    if (paths_.save_root == nullptr) {
        return;
    }
    const auto current = QString::fromStdString(save_root_path().string());
    const QString start =
        QFileInfo(current).isDir() ? current : QFileInfo(current).absolutePath();
    const QString chosen = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("Game Saves Root"),
        start.isEmpty() ? QDir::homePath() : start);
    if (chosen.isEmpty()) {
        return;
    }
    paths_.save_root->setText(chosen);
    update_save_root_status();
    persist_valid_save_root(save_root_path());
}

void MainWindow::create_save_root() {
    if (paths_.save_root == nullptr) {
        return;
    }
    const auto path = save_root_path();
    const QString qpath = QString::fromStdString(path.string());
    if (QFileInfo(qpath).isDir()) {
        persist_valid_save_root(path);
        return;
    }
    if (QFileInfo(qpath).exists()) {
        update_save_root_status();
        append_log(
            settings_log_,
            QStringLiteral("Save root exists but is not a directory: %1").arg(qpath),
            GuiLogLevel::Quiet);
        return;
    }
    if (!QDir().mkpath(qpath)) {
        QString detail = QStringLiteral("Could not create save root: %1").arg(qpath);
        if (running_inside_flatpak()) {
            detail += QStringLiteral(
                " (Flatpak may lack write access — grant with flatpak override "
                "--filesystem=<path>:rw, or create the directory on the host OS)");
        }
        append_log(settings_log_, detail, GuiLogLevel::Quiet);
        update_save_root_status();
        return;
    }
    append_log(settings_log_, QStringLiteral("Created save root: %1").arg(qpath));
    persist_valid_save_root(path);
}

void MainWindow::load_path_settings(QSettings& settings) {
    const auto defaults = default_path_roots();
    if (paths_.art_root != nullptr) {
        paths_.art_root->setText(
            settings.value(QStringLiteral("paths/artRoot"), defaults.art_root).toString());
    }
    if (paths_.rom_root != nullptr) {
        paths_.rom_root->setText(
            settings.value(QStringLiteral("host/romRoot"), defaults.rom_root).toString());
    }
    if (paths_.meta_root != nullptr) {
        paths_.meta_root->setText(
            settings.value(QStringLiteral("host/metaRoot"), defaults.meta_root).toString());
    }
    if (paths_.save_root != nullptr) {
        const QSignalBlocker blocker(paths_.save_root);
        const auto stored = settings.value(QStringLiteral("host/saveRoot")).toString().trimmed();
        // Missing or blank → show the live default, not an empty field.
        paths_.save_root->setText(stored.isEmpty() ? defaults.save_root : stored);
        update_save_root_status();
    }
    if (paths_.native_host_runner != nullptr) {
        const QSignalBlocker blocker(paths_.native_host_runner);
        paths_.native_host_runner->setText(
            settings.value(QStringLiteral("host/nativeHostRunner")).toString());
    }
}

void MainWindow::save_path_settings(QSettings& settings) {
    // Store what the user typed, `~` and all; only resolution expands it.
    settings.setValue(QStringLiteral("paths/artRoot"), path_field_text(paths_.art_root));
    if (paths_.rom_root != nullptr) {
        settings.setValue(QStringLiteral("host/romRoot"), path_field_text(paths_.rom_root));
    }
    if (paths_.meta_root != nullptr) {
        settings.setValue(QStringLiteral("host/metaRoot"), path_field_text(paths_.meta_root));
    }
    if (paths_.save_root != nullptr) {
        // Never persist blank: resolve to the path the host would actually use.
        const QString resolved = QString::fromStdString(save_root_path().string());
        if (paths_.save_root->text().trimmed().isEmpty()) {
            const QSignalBlocker blocker(paths_.save_root);
            paths_.save_root->setText(resolved);
        }
        settings.setValue(QStringLiteral("host/saveRoot"), resolved);
    }
    if (paths_.native_host_runner != nullptr) {
        settings.setValue(
            QStringLiteral("host/nativeHostRunner"),
            path_field_text(paths_.native_host_runner));
    }
}

void MainWindow::apply_art_root_to_pickers() {
    const auto art_root = art_root_path();
    if (host_game_picker_ != nullptr) {
        host_game_picker_->setArtRoot(art_root);
    }
    // Don't overwrite client host-art cache after a successful Connect.
    if (client_game_picker_ != nullptr && !client_catalog_loaded_) {
        client_game_picker_->setArtRoot(art_root);
    }
}

void MainWindow::refresh_art_from_steam() {
    if (!host_runtime_available()) {
        append_log(
            settings_log_,
            "Steam art refresh from a local ROM catalog requires a host-capable build.");
        return;
    }
    if (art_refresh_thread_.joinable()) {
        if (art_refreshing_.load()) {
            append_log(settings_log_, "Art refresh already running.");
            return;
        }
        art_refresh_thread_.join();
    }

    const auto art_root = art_root_path();
    auto rom_root = rom_root_path();
    if (rom_root.empty()) {
        rom_root = art_root.parent_path() / "Games";
    }
    auto meta_root = meta_root_path();
    if (meta_root.empty()) {
        meta_root = art_root.parent_path() / "Meta";
    }
    const auto steam_account_id = steam_account_id_text();

    append_log(
        settings_log_,
        steam_account_id.empty()
            ? "Refreshing art from Steam grid (auto-detect account)..."
            : QString("Refreshing art from Steam account %1...")
                .arg(QString::fromStdString(steam_account_id)));
    art_refreshing_ = true;
    art_refresh_thread_ = std::thread([this, rom_root, meta_root, art_root, steam_account_id] {
        QString message;
        try {
            const auto targets = scan_art_import_targets(rom_root, meta_root);
            archstreamer::SteamArtImportOptions options;
            options.steam_account_id = steam_account_id;
            options.replace_when_different = true;
            const auto result = archstreamer::import_steam_grid_art(targets, art_root, options);
            message = QString(
                "Art refresh done: account=%1 shortcuts=%2 matched=%3 copied=%4 replaced=%5 skipped=%6 unmatched=%7")
                .arg(QString::fromStdString(result.resolved_account_id))
                .arg(result.shortcuts_read)
                .arg(result.matched_games)
                .arg(result.files_copied)
                .arg(result.files_replaced)
                .arg(result.files_skipped)
                .arg(result.unmatched_shortcuts.size());
            if (result.shortcuts_read == 0) {
                message += " (no Steam shortcuts found)";
            }
        } catch (const std::exception& error) {
            message = QString("Art refresh failed: %1").arg(error.what());
        }

        QMetaObject::invokeMethod(
            this,
            [this, message = std::move(message)] {
                art_refreshing_ = false;
                append_log(settings_log_, message);
                QPixmapCache::clear();
                apply_art_root_to_pickers();
                if (host_game_picker_ != nullptr) {
                    host_game_picker_->refreshArtDisplay();
                }
                if (client_game_picker_ != nullptr) {
                    client_game_picker_->refreshArtDisplay();
                }
            },
            Qt::QueuedConnection);
    });
}

} // namespace archstreamer::gui
