#include "main_window.hpp"

#include "gui_logging.hpp"
#include "gui_util.hpp"

#ifdef ARCHSTREAMER_HAS_HOST
#include "host/game_catalog.hpp"
#include "host/game_catalog_scanner.hpp"
#include "host/libretro_core_registry.hpp"
#include "host/save_active_sessions.hpp"
#include "host/save_manager.hpp"
#include "host/save_profile.hpp"
#endif

#include <QAbstractItemView>
#include <QComboBox>
#include <QDir>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

#include <exception>
#include <optional>
#include <unordered_set>

namespace archstreamer::gui {

#ifdef ARCHSTREAMER_HAS_HOST

namespace {

QString expand_user_path_local(QString path) {
    path = path.trimmed();
    if (path == QLatin1String("~")) {
        return QDir::homePath();
    }
    if (path.startsWith(QLatin1String("~/"))) {
        return QDir::homePath() + path.mid(1);
    }
    return path;
}

QString format_bytes(std::uint64_t bytes) {
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (bytes < 1024ull * 1024ull) {
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
}

SaveNameHints hints_from_catalog(const GameCatalog& catalog) {
    SaveNameHints hints;
    for (const auto& game : catalog.list().games) {
        const auto display = QString::fromStdString(game.display_name).trimmed();
        if (display.isEmpty()) {
            continue;
        }
        hints.by_stem[display.toLower().toStdString()] = {game.system_key, game.display_name};
        const auto canonical = QString::fromStdString(game.canonical_name).trimmed().toLower();
        if (!canonical.isEmpty()) {
            hints.by_stem[canonical.toStdString()] = {game.system_key, game.display_name};
        }
        // File stems often match the ROM/content basename more than display_name.
        if (const auto hosted = catalog.find_hosted(game.id); hosted.has_value()) {
            const auto stem =
                QString::fromStdString(hosted->get().content_path.stem().string()).trimmed().toLower();
            if (!stem.isEmpty()) {
                hints.by_stem[stem.toStdString()] = {game.system_key, game.display_name};
            }
        }
    }
    return hints;
}

} // namespace

QWidget* MainWindow::build_saves_tab() {
    auto* page = new QWidget(this);
    auto* root = new QVBoxLayout(page);

    auto* intro = new QLabel(
        "Browse and clean per-user save data under the host save root. "
        "Filter by user and system, then remove a user, an entire system for that user, "
        "or one game. Creating a user seeds the template profile (default password "
        "\"archstreamer\", must change on first login).",
        page);
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto* filters = new QGroupBox("Filters", page);
    auto* filter_form = new QFormLayout(filters);
    saves_root_label_ = new QLabel(filters);
    saves_user_ = new QComboBox(filters);
    saves_system_ = new QComboBox(filters);
    saves_filter_ = new QLineEdit(filters);
    saves_filter_->setPlaceholderText("Filter by game name…");
    filter_form->addRow("Save root", saves_root_label_);
    filter_form->addRow("User", saves_user_);
    filter_form->addRow("System", saves_system_);
    filter_form->addRow("Game filter", saves_filter_);
    root->addWidget(filters);

    auto* actions = new QHBoxLayout();
    saves_refresh_ = new QPushButton("Refresh", page);
    saves_add_user_ = new QPushButton("Add User…", page);
    saves_remove_user_ = new QPushButton("Remove User", page);
    saves_remove_system_ = new QPushButton("Remove System", page);
    saves_remove_game_ = new QPushButton("Remove Game", page);
    actions->addWidget(saves_refresh_);
    actions->addWidget(saves_add_user_);
    actions->addWidget(saves_remove_user_);
    actions->addWidget(saves_remove_system_);
    actions->addWidget(saves_remove_game_);
    actions->addStretch();
    root->addLayout(actions);

    saves_tree_ = new QTreeWidget(page);
    saves_tree_->setColumnCount(5);
    saves_tree_->setHeaderLabels({"User", "System", "Game", "Size", "Status"});
    saves_tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    saves_tree_->setUniformRowHeights(true);
    saves_tree_->setAlternatingRowColors(true);
    saves_tree_->setRootIsDecorated(false);
    saves_tree_->setSortingEnabled(true);
    root->addWidget(saves_tree_, 1);

    saves_status_ = new QLabel("Select a row to enable remove actions.", page);
    saves_status_->setWordWrap(true);
    root->addWidget(saves_status_);

    connect(saves_refresh_, &QPushButton::clicked, this, [this] { refresh_saves_browser(); });
    connect(saves_user_, &QComboBox::currentIndexChanged, this, [this] {
        refresh_saves_system_combo();
        refresh_saves_browser_list();
    });
    connect(saves_system_, &QComboBox::currentIndexChanged, this, [this] {
        refresh_saves_browser_list();
    });
    connect(saves_filter_, &QLineEdit::textChanged, this, [this] { refresh_saves_browser_list(); });
    connect(saves_tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
        update_saves_action_enabled();
    });
    connect(saves_add_user_, &QPushButton::clicked, this, [this] { saves_add_user(); });
    connect(saves_remove_user_, &QPushButton::clicked, this, [this] { saves_remove_user(); });
    connect(saves_remove_system_, &QPushButton::clicked, this, [this] { saves_remove_system(); });
    connect(saves_remove_game_, &QPushButton::clicked, this, [this] { saves_remove_game(); });

    QTimer::singleShot(0, this, [this] { refresh_saves_browser(); });
    return page;
}

bool MainWindow::saves_host_busy() const {
    return host_process_ != nullptr && host_process_->state() != QProcess::NotRunning;
}

SaveNameHints MainWindow::saves_name_hints() const {
    return saves_hints_;
}

void MainWindow::refresh_saves_browser() {
    if (saves_root_label_ == nullptr) {
        return;
    }
    const auto root = save_root_path();
    saves_root_label_->setText(QString::fromStdString(root.string()));

    saves_hints_ = {};
    try {
        const auto rom = host_rom_root_ != nullptr ? host_rom_root_->text().trimmed() : QString();
        if (!rom.isEmpty()) {
            std::filesystem::path meta;
            if (host_meta_root_ != nullptr && !host_meta_root_->text().trimmed().isEmpty()) {
                meta = expand_user_path_local(host_meta_root_->text()).toStdString();
            }
            const auto catalog = scan_game_catalog(
                expand_user_path_local(rom).toStdString(),
                LibretroCoreRegistry::ubuntu_defaults(),
                meta);
            saves_hints_ = hints_from_catalog(catalog);
        }
    } catch (const std::exception& error) {
        saves_status_->setText(
            QStringLiteral("Catalog hints unavailable: %1").arg(error.what()));
    }

    const auto users = list_save_users(root);
    const QString previous_user = saves_user_->currentData().toString();
    {
        const QSignalBlocker block(saves_user_);
        saves_user_->clear();
        saves_user_->addItem("All users", QString());
        for (const auto& user : users) {
            saves_user_->addItem(QString::fromStdString(user), QString::fromStdString(user));
        }
        const int idx = saves_user_->findData(previous_user);
        saves_user_->setCurrentIndex(idx >= 0 ? idx : 0);
    }

    refresh_saves_system_combo();
    refresh_saves_browser_list();
}

void MainWindow::refresh_saves_system_combo() {
    if (saves_system_ == nullptr) {
        return;
    }
    const auto root = save_root_path();
    const auto user = saves_user_->currentData().toString().toStdString();
    const auto systems = list_save_systems(root, user, saves_hints_);
    const QString previous = saves_system_->currentData().toString();
    const QSignalBlocker block(saves_system_);
    saves_system_->clear();
    saves_system_->addItem("All systems", QString());
    for (const auto& key : systems) {
        saves_system_->addItem(
            QString::fromStdString(save_system_label(key)),
            QString::fromStdString(key));
    }
    const int idx = saves_system_->findData(previous);
    saves_system_->setCurrentIndex(idx >= 0 ? idx : 0);
}

void MainWindow::refresh_saves_browser_list() {
    if (saves_tree_ == nullptr) {
        return;
    }
    const auto root = save_root_path();
    const auto user = saves_user_->currentData().toString().toStdString();
    const auto system = saves_system_->currentData().toString().toStdString();
    const auto filter = saves_filter_->text().trimmed().toLower();

    const auto games = list_save_games(root, user, system, saves_hints_);
    const auto active_sessions = list_active_save_sessions(root);
    // Key by user+game — title-id / file keys are shared across profiles.
    std::unordered_set<std::string> active_user_games;
    for (const auto& active : active_sessions) {
        if (const auto key = best_active_game_key(games, active); key.has_value()) {
            active_user_games.insert(active.username + '\n' + *key);
        }
    }

    saves_tree_->setSortingEnabled(false);
    saves_tree_->clear();
    int shown = 0;
    int active_shown = 0;
    for (const auto& game : games) {
        const auto name = QString::fromStdString(game.display_name);
        if (!filter.isEmpty() && !name.toLower().contains(filter)
            && !QString::fromStdString(game.username).toLower().contains(filter)
            && !QString::fromStdString(game.system_label).toLower().contains(filter)) {
            continue;
        }
        const bool active = active_user_games.contains(game.username + '\n' + game.game_key);
        auto* item = new QTreeWidgetItem(saves_tree_);
        item->setText(0, QString::fromStdString(game.username));
        item->setText(1, QString::fromStdString(game.system_label));
        item->setText(2, name);
        item->setText(3, format_bytes(game.bytes));
        item->setText(4, active ? QStringLiteral("Active") : QString());
        item->setData(0, Qt::UserRole, QString::fromStdString(game.username));
        item->setData(1, Qt::UserRole, QString::fromStdString(game.system_key));
        item->setData(2, Qt::UserRole, QString::fromStdString(game.game_key));
        item->setToolTip(2, QString::fromStdString(game.primary_path.string()));
        if (active) {
            item->setToolTip(4, QStringLiteral("This user is in a live host session on this game."));
            ++active_shown;
        }
        ++shown;
    }
    saves_tree_->setSortingEnabled(true);
    saves_tree_->resizeColumnToContents(0);
    saves_tree_->resizeColumnToContents(1);
    saves_tree_->resizeColumnToContents(3);
    saves_tree_->resizeColumnToContents(4);
    QString status = QStringLiteral("%1 save(s) shown under %2")
        .arg(shown)
        .arg(QString::fromStdString(root.string()));
    if (active_shown > 0) {
        status += QStringLiteral(" — %1 active").arg(active_shown);
    }
    saves_status_->setText(status);
    update_saves_action_enabled();
}

void MainWindow::update_saves_action_enabled() {
    const auto* item = saves_tree_ != nullptr ? saves_tree_->currentItem() : nullptr;
    const bool has_row = item != nullptr;
    const bool has_user_filter = saves_user_ != nullptr && !saves_user_->currentData().toString().isEmpty();
    const bool has_system_filter =
        saves_system_ != nullptr && !saves_system_->currentData().toString().isEmpty();
    if (saves_remove_game_ != nullptr) {
        saves_remove_game_->setEnabled(has_row);
    }
    if (saves_remove_user_ != nullptr) {
        saves_remove_user_->setEnabled(has_row || has_user_filter);
    }
    if (saves_remove_system_ != nullptr) {
        saves_remove_system_->setEnabled(has_row || (has_user_filter && has_system_filter));
    }
}

bool MainWindow::confirm_saves_destructive(const QString& title, const QString& detail) {
    if (saves_host_busy()) {
        const auto answer = QMessageBox::warning(
            this,
            title,
            detail
                + "\n\nHost Runner is currently running. Deleting saves for an active "
                  "player can corrupt that session. Stop Host first unless you are sure.",
            QMessageBox::Ok | QMessageBox::Cancel,
            QMessageBox::Cancel);
        return answer == QMessageBox::Ok;
    }
    return QMessageBox::question(this, title, detail, QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        == QMessageBox::Yes;
}

void MainWindow::saves_add_user() {
    bool ok = false;
    const auto name = QInputDialog::getText(
                          this,
                          "Add User",
                          "Username (letters, digits, _-):",
                          QLineEdit::Normal,
                          {},
                          &ok)
                          .trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }
    try {
        create_save_user(save_root_path(), name.toStdString());
        saves_status_->setText(
            QStringLiteral("Created user “%1” (default password archstreamer, must change).")
                .arg(name));
        append_log(host_log_, QStringLiteral("[saves] created user %1").arg(name));
        refresh_saves_browser();
        const int idx = saves_user_->findData(name);
        if (idx >= 0) {
            saves_user_->setCurrentIndex(idx);
        }
    } catch (const std::exception& error) {
        QMessageBox::warning(this, "Add User", error.what());
    }
}

void MainWindow::saves_remove_user() {
    QString username;
    if (const auto* item = saves_tree_->currentItem()) {
        username = item->data(0, Qt::UserRole).toString();
    }
    if (username.isEmpty()) {
        username = saves_user_->currentData().toString();
    }
    if (username.isEmpty()) {
        QMessageBox::information(this, "Remove User", "Select a user (filter or row) first.");
        return;
    }
    if (!confirm_saves_destructive(
            "Remove User",
            QStringLiteral(
                "Permanently delete all save data for user “%1”?\n\n"
                "This removes the entire profile directory under the save root.")
                .arg(username))) {
        return;
    }
    try {
        delete_save_user(save_root_path(), username.toStdString());
        saves_status_->setText(QStringLiteral("Deleted user “%1”.").arg(username));
        append_log(host_log_, QStringLiteral("[saves] deleted user %1").arg(username));
        refresh_saves_browser();
    } catch (const std::exception& error) {
        QMessageBox::warning(this, "Remove User", error.what());
    }
}

void MainWindow::saves_remove_system() {
    QString username;
    QString system_key;
    QString system_label;
    if (const auto* item = saves_tree_->currentItem()) {
        username = item->data(0, Qt::UserRole).toString();
        system_key = item->data(1, Qt::UserRole).toString();
        system_label = item->text(1);
    }
    if (username.isEmpty()) {
        username = saves_user_->currentData().toString();
    }
    if (system_key.isEmpty()) {
        system_key = saves_system_->currentData().toString();
        system_label = saves_system_->currentText();
    }
    if (username.isEmpty() || system_key.isEmpty()) {
        QMessageBox::information(
            this,
            "Remove System",
            "Select a save row, or set both User and System filters.");
        return;
    }
    if (!confirm_saves_destructive(
            "Remove System",
            QStringLiteral(
                "Delete all “%1” saves for user “%2”?\n\n"
                "Switch removals also clear Ryujinx/Yuzu mirrors for those titles.")
                .arg(system_label, username))) {
        return;
    }
    try {
        const auto count = delete_save_system(
            save_root_path(),
            username.toStdString(),
            system_key.toStdString(),
            saves_hints_);
        saves_status_->setText(
            QStringLiteral("Removed %1 “%2” save(s) for %3.")
                .arg(count)
                .arg(system_label, username));
        append_log(
            host_log_,
            QStringLiteral("[saves] deleted system %1 for %2 (%3 entries)")
                .arg(system_key, username)
                .arg(count));
        refresh_saves_browser();
    } catch (const std::exception& error) {
        QMessageBox::warning(this, "Remove System", error.what());
    }
}

void MainWindow::saves_remove_game() {
    const auto* item = saves_tree_->currentItem();
    if (item == nullptr) {
        QMessageBox::information(this, "Remove Game", "Select a game row first.");
        return;
    }
    const auto username = item->data(0, Qt::UserRole).toString();
    const auto game_key = item->data(2, Qt::UserRole).toString();
    const auto label = item->text(2);
    if (!confirm_saves_destructive(
            "Remove Game",
            QStringLiteral("Delete save “%1” for user “%2”?").arg(label, username))) {
        return;
    }
    try {
        delete_save_game(save_root_path(), username.toStdString(), game_key.toStdString());
        saves_status_->setText(QStringLiteral("Deleted “%1” for %2.").arg(label, username));
        append_log(
            host_log_,
            QStringLiteral("[saves] deleted game %1 for %2").arg(game_key, username));
        refresh_saves_browser();
    } catch (const std::exception& error) {
        QMessageBox::warning(this, "Remove Game", error.what());
    }
}

#endif // ARCHSTREAMER_HAS_HOST

} // namespace archstreamer::gui
