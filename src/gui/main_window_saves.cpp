#include "main_window.hpp"

#include "gui_logging.hpp"
#include "gui_util.hpp"

#ifdef ARCHSTREAMER_HAS_HOST
#include "archstreamer/runtime_cadence/cadence.hpp"
#include "host/game_catalog.hpp"
#include "host/game_catalog_scanner.hpp"
#include "host/game_meta_store.hpp"
#include "host/libretro_core_registry.hpp"
#include "host/save_active_sessions.hpp"
#include "host/save_manager.hpp"
#include "host/save_profile.hpp"
#include "host/ps2_memcard.hpp"
#endif

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <exception>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace archstreamer::gui {

#ifdef ARCHSTREAMER_HAS_HOST

namespace {

constexpr int kUserRoleUsername = Qt::UserRole;
constexpr int kUserRoleSystemKey = Qt::UserRole + 1;
constexpr int kUserRoleGameKey = Qt::UserRole + 2;
constexpr int kUserRoleSlot = Qt::UserRole + 3;
constexpr int kUserRoleSynthetic = Qt::UserRole + 4;
constexpr int kUserRoleNodeKind = Qt::UserRole + 5;
constexpr int kUserRoleSortBytes = Qt::UserRole + 6;
constexpr int kUserRoleCatalogGameId = Qt::UserRole + 7;
constexpr int kBlockedRoleGameId = Qt::UserRole;

enum class SavesNodeKind { User = 0, System = 1, Game = 2 };

/** Sibling-level sort: Name/Status by text, Size by stored byte count. */
class SavesTreeItem final : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem& other) const override {
        const int col = treeWidget() != nullptr ? treeWidget()->sortColumn() : 0;
        if (col == 1) {
            return data(1, kUserRoleSortBytes).toULongLong()
                < other.data(1, kUserRoleSortBytes).toULongLong();
        }
        return QString::compare(text(col), other.text(col), Qt::CaseInsensitive) < 0;
    }
};

void apply_saves_node_roles(
    QTreeWidgetItem* item,
    SavesNodeKind kind,
    const QString& username,
    const QString& system_key = {},
    const QString& game_key = {},
    bool synthetic = false,
    int slot = -1) {
    item->setData(0, kUserRoleNodeKind, static_cast<int>(kind));
    item->setData(0, kUserRoleUsername, username);
    item->setData(0, kUserRoleSystemKey, system_key);
    item->setData(0, kUserRoleGameKey, game_key);
    item->setData(0, kUserRoleSynthetic, synthetic);
    if (slot >= 0) {
        item->setData(0, kUserRoleSlot, slot);
    }
}

QSet<QString> collect_expanded_saves_keys(QTreeWidget* tree) {
    QSet<QString> keys;
    if (tree == nullptr) {
        return keys;
    }
    for (int u = 0; u < tree->topLevelItemCount(); ++u) {
        auto* user_item = tree->topLevelItem(u);
        const auto username = user_item->data(0, kUserRoleUsername).toString();
        if (user_item->isExpanded()) {
            keys.insert(username);
        }
        for (int s = 0; s < user_item->childCount(); ++s) {
            auto* system_item = user_item->child(s);
            if (system_item->isExpanded()) {
                keys.insert(
                    username + QLatin1Char('\n') + system_item->data(0, kUserRoleSystemKey).toString());
            }
        }
    }
    return keys;
}

/** Stable identity for restoring selection after a Users tree rebuild. */
QString saves_item_identity(const QTreeWidgetItem* item) {
    if (item == nullptr) {
        return {};
    }
    const int kind = item->data(0, kUserRoleNodeKind).toInt();
    const auto username = item->data(0, kUserRoleUsername).toString();
    const auto system_key = item->data(0, kUserRoleSystemKey).toString();
    const auto game_key = item->data(0, kUserRoleGameKey).toString();
    const bool synthetic = item->data(0, kUserRoleSynthetic).toBool();
    const int slot = item->data(0, kUserRoleSlot).isValid() ? item->data(0, kUserRoleSlot).toInt() : -1;
    QString id = QString::number(kind) + QLatin1Char('\n') + username + QLatin1Char('\n') + system_key
        + QLatin1Char('\n') + game_key + QLatin1Char('\n')
        + (synthetic ? QLatin1Char('1') : QLatin1Char('0')) + QLatin1Char('\n')
        + QString::number(slot);
    // Synthetic "Playing:" rows often lack a game_key — keep display text for match.
    if (synthetic && game_key.isEmpty()) {
        id += QLatin1Char('\n') + item->text(0);
    }
    return id;
}

QString collect_selected_saves_identity(QTreeWidget* tree) {
    if (tree == nullptr) {
        return {};
    }
    return saves_item_identity(tree->currentItem());
}

void restore_selected_saves_identity(QTreeWidget* tree, const QString& identity) {
    if (tree == nullptr || identity.isEmpty()) {
        return;
    }
    std::vector<QTreeWidgetItem*> stack;
    stack.reserve(static_cast<std::size_t>(tree->topLevelItemCount()));
    for (int u = 0; u < tree->topLevelItemCount(); ++u) {
        stack.push_back(tree->topLevelItem(u));
    }
    while (!stack.empty()) {
        auto* item = stack.back();
        stack.pop_back();
        if (item == nullptr) {
            continue;
        }
        if (saves_item_identity(item) == identity) {
            tree->setCurrentItem(item);
            item->setSelected(true);
            return;
        }
        for (int i = item->childCount() - 1; i >= 0; --i) {
            stack.push_back(item->child(i));
        }
    }
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
        // Cadence sessions store catalog game ids — map those to display names too.
        if (!game.id.empty()) {
            hints.by_stem[game.id] = {game.system_key, game.display_name};
        }
    }
    return hints;
}

SaveNameHints hints_from_meta_store() {
    try {
        GameMetaStore meta;
        if (meta.ready()) {
            return meta.save_name_hints();
        }
    } catch (...) {
    }
    return {};
}

/**
 * Active play sessions for the Users tab.
 * Cadence `sessions` (ended_at=0) is the source of truth.
 */
std::vector<ActiveSaveSession> load_users_active_sessions(
    const std::filesystem::path& save_root) {
    std::vector<ActiveSaveSession> out;
    try {
        auto store = cadence::make_runtime_store();
        if (store && store->ensure_ready()) {
            for (const auto& session : store->list_sessions(true)) {
                if (session.username.empty()) {
                    continue;
                }
                ActiveSaveSession active;
                active.username = session.username;
                active.game_id = session.game_key;
                active.system_key = session.system_key;
                active.slot_index = session.slot;
                active.display_name = session.game_key;
                out.push_back(std::move(active));
            }
        }
    } catch (...) {
    }

    if (!out.empty()) {
        return out;
    }

    // Legacy fallback: older hosts may still have side JSON only.
    return list_active_save_sessions(save_root);
}

QString resolve_active_display_name(
    const ActiveSaveSession& active,
    const SaveNameHints& hints) {
    // Meta DB is authoritative for catalog ids / aliases.
    if (!active.game_id.empty() || !active.display_name.empty() || !active.content_path.empty()) {
        try {
            GameMetaStore meta;
            if (meta.ready()) {
                if (!active.game_id.empty()) {
                    if (const auto row = meta.resolve(active.game_id, active.system_key)) {
                        return QString::fromStdString(row->display_name);
                    }
                }
                if (!active.content_path.empty()) {
                    const auto stem =
                        QString::fromStdString(
                            std::filesystem::path(active.content_path).stem().string())
                            .trimmed();
                    if (!stem.isEmpty()) {
                        if (const auto row =
                                meta.resolve(stem.toStdString(), active.system_key)) {
                            return QString::fromStdString(row->display_name);
                        }
                    }
                }
                if (!active.display_name.empty()) {
                    if (const auto row =
                            meta.resolve(active.display_name, active.system_key)) {
                        return QString::fromStdString(row->display_name);
                    }
                }
            }
        } catch (...) {
        }
    }
    if (!active.display_name.empty() && active.display_name != active.game_id) {
        return QString::fromStdString(active.display_name);
    }
    if (!active.game_id.empty()) {
        if (const auto it = hints.by_stem.find(active.game_id); it != hints.by_stem.end()) {
            return QString::fromStdString(it->second.second);
        }
        return QString::fromStdString(active.game_id);
    }
    return QStringLiteral("(unknown game)");
}

} // namespace

QWidget* MainWindow::build_saves_tab() {
    auto* page = new QWidget(this);
    auto* root = new QVBoxLayout(page);

    auto* intro = new QLabel(
        "Per-user save profiles under the host save root, grouped User → System → Game. "
        "Right-click for Add / Remove / Kick / Block. "
        "Status sits on the user (Connected) or game (Active). "
        "Blocked games are hidden from that user's catalog after they connect.",
        page);
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto* filters = new QGroupBox("Filters", page);
    auto* filter_form = new QFormLayout(filters);
    saves_root_label_ = new QLabel(filters);
    saves_user_ = new QComboBox(filters);
    saves_system_ = new QComboBox(filters);
    saves_filter_ = new QLineEdit(filters);
    saves_filter_->setPlaceholderText("Filter by user, system, or game…");
    filter_form->addRow("Save root", saves_root_label_);
    filter_form->addRow("User", saves_user_);
    filter_form->addRow("System", saves_system_);
    filter_form->addRow("Filter", saves_filter_);
    root->addWidget(filters);

    auto* actions = new QHBoxLayout();
    saves_refresh_ = new QPushButton("Refresh", page);
    auto* saves_expand_all = new QPushButton("Expand all", page);
    auto* saves_collapse_all = new QPushButton("Collapse all", page);
    saves_expand_all->setToolTip("Expand every user and system in the list.");
    saves_collapse_all->setToolTip("Collapse every user and system in the list.");
    actions->addWidget(saves_refresh_);
    actions->addWidget(saves_expand_all);
    actions->addWidget(saves_collapse_all);
    actions->addStretch();
    root->addLayout(actions);

    auto* split = new QSplitter(Qt::Horizontal, page);

    saves_tree_ = new QTreeWidget(split);
    saves_tree_->setColumnCount(3);
    saves_tree_->setHeaderLabels({"Name", "Size", "Status"});
    saves_tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    saves_tree_->setUniformRowHeights(true);
    saves_tree_->setAlternatingRowColors(true);
    saves_tree_->setRootIsDecorated(true);
    saves_tree_->setItemsExpandable(true);
    saves_tree_->setAnimated(true);
    saves_tree_->setSortingEnabled(true);
    saves_tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    {
        auto* header = saves_tree_->header();
        header->setSectionsClickable(true);
        header->setSortIndicatorShown(true);
        header->setStretchLastSection(false);
        header->setCascadingSectionResizes(false);
        header->setMinimumSectionSize(72);
        header->setSectionResizeMode(0, QHeaderView::Stretch);
        header->setSectionResizeMode(1, QHeaderView::Interactive);
        header->setSectionResizeMode(2, QHeaderView::Interactive);
        header->resizeSection(1, 100);
        header->resizeSection(2, 120);
    }
    saves_tree_->sortByColumn(0, Qt::AscendingOrder);

    auto* blocked_panel = new QWidget(split);
    auto* blocked_layout = new QVBoxLayout(blocked_panel);
    blocked_layout->setContentsMargins(0, 0, 0, 0);
    saves_blocked_label_ = new QLabel("Blocked games", blocked_panel);
    saves_blocked_label_->setWordWrap(true);
    saves_blocked_list_ = new QListWidget(blocked_panel);
    saves_blocked_list_->setAlternatingRowColors(true);
    saves_blocked_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    saves_blocked_list_->setContextMenuPolicy(Qt::CustomContextMenu);
    blocked_layout->addWidget(saves_blocked_label_);
    blocked_layout->addWidget(saves_blocked_list_, 1);

    split->addWidget(saves_tree_);
    split->addWidget(blocked_panel);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    root->addWidget(split, 1);

    saves_status_ = new QLabel(
        "Right-click a game to Block it for that user. Kick is also on the context menu.",
        page);
    saves_status_->setWordWrap(true);
    root->addWidget(saves_status_);

    saves_add_user_action_ = new QAction("User…", page);
    saves_remove_user_action_ = new QAction("User…", page);
    saves_remove_system_action_ = new QAction("System…", page);
    saves_remove_game_action_ = new QAction("Game…", page);
    saves_kick_action_ = new QAction("Kick…", page);
    saves_block_game_action_ = new QAction("Block game for user", page);
    saves_unblock_game_action_ = new QAction("Unblock game", page);
    auto* saves_copy_action = new QAction("Copy", page);
    saves_copy_action->setShortcut(QKeySequence::Copy);
    saves_copy_action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    saves_tree_->addAction(saves_copy_action);

    connect(saves_refresh_, &QPushButton::clicked, this, [this] { refresh_saves_browser(); });
    connect(saves_expand_all, &QPushButton::clicked, this, [this] {
        if (saves_tree_ != nullptr) {
            saves_tree_->expandAll();
        }
    });
    connect(saves_collapse_all, &QPushButton::clicked, this, [this] {
        if (saves_tree_ != nullptr) {
            saves_tree_->collapseAll();
        }
    });
    connect(saves_user_, &QComboBox::currentIndexChanged, this, [this] {
        refresh_saves_system_combo();
        refresh_saves_browser_list();
        refresh_saves_blocked_list();
    });
    connect(saves_system_, &QComboBox::currentIndexChanged, this, [this] {
        refresh_saves_browser_list();
    });
    connect(saves_filter_, &QLineEdit::textChanged, this, [this] { refresh_saves_browser_list(); });
    connect(saves_tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
        update_saves_action_enabled();
        refresh_saves_blocked_list();
    });
    connect(saves_tree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        saves_show_context_menu(pos);
    });
    connect(saves_blocked_list_, &QListWidget::itemSelectionChanged, this, [this] {
        update_saves_action_enabled();
    });
    connect(saves_blocked_list_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        if (saves_blocked_list_ == nullptr) {
            return;
        }
        if (auto* under = saves_blocked_list_->itemAt(pos)) {
            saves_blocked_list_->setCurrentItem(under);
        }
        update_saves_action_enabled();
        saves_context_menu_open_ = true;
        QMenu menu(saves_blocked_list_);
        connect(&menu, &QMenu::aboutToHide, this, [this] { saves_context_menu_open_ = false; });
        menu.addAction(saves_unblock_game_action_);
        menu.exec(saves_blocked_list_->viewport()->mapToGlobal(pos));
        saves_context_menu_open_ = false;
    });
    connect(saves_copy_action, &QAction::triggered, this, [this] { saves_copy_selection(); });
    connect(saves_add_user_action_, &QAction::triggered, this, [this] { saves_add_user(); });
    connect(saves_remove_user_action_, &QAction::triggered, this, [this] { saves_remove_user(); });
    connect(saves_remove_system_action_, &QAction::triggered, this, [this] { saves_remove_system(); });
    connect(saves_remove_game_action_, &QAction::triggered, this, [this] { saves_remove_game(); });
    connect(saves_kick_action_, &QAction::triggered, this, [this] { saves_kick_user(); });
    connect(saves_block_game_action_, &QAction::triggered, this, [this] {
        saves_block_selected_game();
    });
    connect(saves_unblock_game_action_, &QAction::triggered, this, [this] {
        saves_unblock_selected_blocked_game();
    });

    saves_refresh_timer_ = new QTimer(page);
    saves_refresh_timer_->setInterval(3000);
    connect(saves_refresh_timer_, &QTimer::timeout, this, [this] {
        if (tabs_ == nullptr || saves_tree_ == nullptr) {
            return;
        }
        if (saves_context_menu_open_) {
            return;
        }
        if (tabs_->tabText(tabs_->currentIndex()) != QLatin1String("Users")) {
            return;
        }
        refresh_saves_browser_list();
    });
    saves_refresh_timer_->start();

    // Users is never the startup tab; kick off the one-shot PS2 scan on first entry.
    // Re-entry still refreshes Connected/Active.
    if (tabs_ != nullptr) {
        connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
            if (tabs_ == nullptr || tabs_->tabText(index) != QLatin1String("Users")) {
                return;
            }
            const bool first_entry = !ps2_prewarm_started_;
            start_ps2_memcard_prewarm();
            if (first_entry) {
                refresh_saves_browser();
                return;
            }
            refresh_saves_browser_list();
        });
    }
    return page;
}

void MainWindow::start_ps2_memcard_prewarm() {
    if (ps2_prewarm_started_) {
        return;
    }
    ps2_prewarm_started_ = true;
    ps2_prewarm_running_ = true;

    // Parsing every user's memcard images takes seconds; keep it off the GUI
    // thread so the tree paints now and gains PS2 sizes once this lands.
    ps2_prewarm_thread_ = std::thread([this, root = save_root_path()] {
        QString failure;
        try {
            ps2_memcard_prewarm(list_ps2_memcard_images(root));
        } catch (const std::exception& error) {
            failure = QString::fromUtf8(error.what());
        }
        QMetaObject::invokeMethod(
            this,
            [this, failure = std::move(failure)] {
                ps2_prewarm_running_ = false;
                if (!failure.isEmpty()) {
                    append_log(
                        host_log_,
                        QStringLiteral("[users] PS2 memory card scan failed: %1").arg(failure),
                        GuiLogLevel::Quiet);
                }
                refresh_saves_browser_list();
            },
            Qt::QueuedConnection);
    });
}

bool MainWindow::saves_host_busy() const {
    return host_process_ != nullptr && host_process_->state() != QProcess::NotRunning;
}

SaveNameHints MainWindow::saves_name_hints() const {
    return saves_hints_;
}

void MainWindow::saves_show_context_menu(const QPoint& pos) {
    if (saves_tree_ == nullptr) {
        return;
    }
    // Right-click should target the row under the cursor (and keep it selected).
    if (auto* under = saves_tree_->itemAt(pos)) {
        saves_tree_->setCurrentItem(under);
        if (!under->isSelected()) {
            saves_tree_->clearSelection();
            under->setSelected(true);
        }
    }
    update_saves_action_enabled();

    saves_context_menu_open_ = true;
    QMenu menu(saves_tree_);
    connect(&menu, &QMenu::aboutToHide, this, [this] { saves_context_menu_open_ = false; });
    auto* copy_action = menu.addAction("Copy");
    copy_action->setShortcut(QKeySequence::Copy);
    copy_action->setEnabled(!saves_tree_->selectedItems().isEmpty());
    connect(copy_action, &QAction::triggered, this, [this] { saves_copy_selection(); });
    menu.addSeparator();
    auto* add_menu = menu.addMenu("Add");
    add_menu->addAction(saves_add_user_action_);
    auto* remove_menu = menu.addMenu("Remove");
    remove_menu->addAction(saves_remove_user_action_);
    remove_menu->addAction(saves_remove_system_action_);
    remove_menu->addAction(saves_remove_game_action_);
    menu.addSeparator();
    menu.addAction(saves_block_game_action_);
    menu.addAction(saves_kick_action_);
    menu.exec(saves_tree_->viewport()->mapToGlobal(pos));
    saves_context_menu_open_ = false;
}

void MainWindow::saves_copy_selection() {
    if (saves_tree_ == nullptr) {
        return;
    }
    const auto selected = saves_tree_->selectedItems();
    if (selected.isEmpty()) {
        return;
    }
    QStringList lines;
    for (const auto* item : selected) {
        if (item == nullptr) {
            continue;
        }
        const auto kind = item->data(0, kUserRoleNodeKind).toInt();
        const auto username = item->data(0, kUserRoleUsername).toString();
        const auto system_key = item->data(0, kUserRoleSystemKey).toString();
        const auto game_key = item->data(0, kUserRoleGameKey).toString();
        const auto name = item->text(0);
        const auto size = item->text(1);
        const auto status = item->text(2);
        const auto path = item->toolTip(0);

        QStringList parts;
        if (kind == static_cast<int>(SavesNodeKind::User)) {
            parts << username;
        } else if (kind == static_cast<int>(SavesNodeKind::System)) {
            parts << username << name;
        } else {
            parts << username;
            if (!system_key.isEmpty()) {
                parts << QString::fromStdString(save_system_label(system_key.toStdString()));
            }
            parts << name;
            if (!size.isEmpty()) {
                parts << size;
            }
            if (!status.isEmpty()) {
                parts << status;
            }
            if (!game_key.isEmpty()) {
                parts << game_key;
            }
            if (!path.isEmpty()) {
                parts << path;
            }
        }
        lines << parts.join(QLatin1Char('\t'));
    }
    if (lines.isEmpty()) {
        return;
    }
    QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
    if (saves_status_ != nullptr) {
        saves_status_->setText(
            QStringLiteral("Copied %1 selected row(s) to the clipboard.").arg(lines.size()));
    }
}

void MainWindow::refresh_saves_browser() {
    if (saves_root_label_ == nullptr) {
        return;
    }
    const auto root = save_root_path();
    saves_root_label_->setText(QString::fromStdString(root.string()));

    saves_hints_ = {};
    try {
        const auto rom = rom_root_path();
        std::optional<GameCatalog> scanned;
        if (!rom.empty()) {
            // Syncs game_meta; Users tab treats the meta DB as authoritative.
            scanned = scan_game_catalog(
                rom,
                LibretroCoreRegistry::ubuntu_defaults(),
                meta_root_path());
        }
        saves_hints_ = hints_from_meta_store();
        // Bootstrap only when the meta DB is empty (first run / open failure).
        if (saves_hints_.by_stem.empty() && scanned.has_value()) {
            saves_hints_ = hints_from_catalog(*scanned);
        }
    } catch (const std::exception& error) {
        saves_hints_ = hints_from_meta_store();
        if (saves_hints_.by_stem.empty()) {
            saves_status_->setText(
                QStringLiteral("Catalog / meta hints unavailable: %1").arg(error.what()));
        }
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
    // Rebuilding the tree clears selection and disables shared context-menu actions.
    if (saves_context_menu_open_) {
        return;
    }
    const auto root = save_root_path();
    const auto user = saves_user_->currentData().toString().toStdString();
    const auto system = saves_system_->currentData().toString().toStdString();
    const auto filter = saves_filter_->text().trimmed().toLower();
    const auto previously_expanded = collect_expanded_saves_keys(saves_tree_);
    const auto previously_selected = collect_selected_saves_identity(saves_tree_);
    const int previous_vscroll =
        saves_tree_->verticalScrollBar() != nullptr ? saves_tree_->verticalScrollBar()->value() : 0;
    const int previous_hscroll =
        saves_tree_->horizontalScrollBar() != nullptr ? saves_tree_->horizontalScrollBar()->value() : 0;

    const auto games = list_save_games(root, user, system, saves_hints_);
    const auto active_sessions = load_users_active_sessions(root);
    const auto connected_clients = list_connected_clients(root);

    std::unordered_set<std::string> active_usernames;
    for (const auto& active : active_sessions) {
        active_usernames.insert(active.username);
    }

    // Connected usernames that are not currently Active (playing).
    std::unordered_set<std::string> connected_usernames;
    std::unordered_map<std::string, ConnectedClientPresence> connected_by_user;
    for (const auto& client : connected_clients) {
        if (active_usernames.contains(client.username)) {
            continue;
        }
        connected_usernames.insert(client.username);
        connected_by_user.emplace(client.username, client);
    }

    // Key by user+game — title-id / file keys are shared across profiles.
    std::unordered_set<std::string> active_user_games;
    std::unordered_map<std::string, ActiveSaveSession> active_by_user_game;
    std::vector<ActiveSaveSession> unmatched_actives;
    for (const auto& active : active_sessions) {
        if (const auto key = best_active_game_key(games, active); key.has_value()) {
            const auto map_key = active.username + '\n' + *key;
            active_user_games.insert(map_key);
            active_by_user_game[map_key] = active;
        } else {
            unmatched_actives.push_back(active);
        }
    }

    auto passes_filter = [&](const QString& username,
                             const QString& system_label,
                             const QString& game_name) {
        if (filter.isEmpty()) {
            return true;
        }
        return username.toLower().contains(filter) || system_label.toLower().contains(filter)
            || game_name.toLower().contains(filter);
    };

    struct PendingGameRow {
        QString username;
        QString system_key;
        QString system_label;
        QString game_key;
        QString catalog_game_id;
        QString display_name;
        QString size_text;
        std::uint64_t bytes = 0;
        QString path_tip;
        bool active = false;
        bool synthetic = false;
        bool save_stem_mismatch = false;
        QString expected_save_stem;
        int slot = -1;
        QString status_tip;
    };

    std::vector<PendingGameRow> pending;
    pending.reserve(games.size() + unmatched_actives.size());

    for (const auto& game : games) {
        const auto username_q = QString::fromStdString(game.username);
        const auto system_label_q = QString::fromStdString(game.system_label);
        const auto name = QString::fromStdString(game.display_name);
        if (!passes_filter(username_q, system_label_q, name)) {
            continue;
        }
        PendingGameRow row;
        row.username = username_q;
        row.system_key = QString::fromStdString(game.system_key);
        row.system_label = system_label_q;
        row.game_key = QString::fromStdString(game.game_key);
        if (!game.catalog_game_id.empty()) {
            row.catalog_game_id = QString::fromStdString(game.catalog_game_id);
        } else if (is_ps2_meta_game_key(game.game_key)) {
            row.catalog_game_id = QString::fromStdString(game_id_from_ps2_meta_key(game.game_key));
        }
        row.save_stem_mismatch = game.save_stem_mismatch;
        row.expected_save_stem = QString::fromStdString(game.expected_save_stem);
        row.display_name = name;
        if (game.system_key == "ps2" && !ps2_memcard_scan_complete()) {
            // PS2 sizes come out of the memcard images. Until the background scan
            // lands they would all read "0 B", which looks like an empty save.
            row.size_text = QStringLiteral("…");
        } else if (game.capacity_bytes > 0) {
            row.size_text = QStringLiteral("%1 / %2")
                .arg(format_bytes(game.bytes), format_bytes(game.capacity_bytes));
        } else {
            row.size_text = format_bytes(game.bytes);
        }
        row.bytes = game.bytes;
        row.path_tip = QString::fromStdString(game.primary_path.string());
        if (game.save_stem_mismatch && !game.expected_save_stem.empty()) {
            row.path_tip += QStringLiteral(
                "\n\nSave name mismatch: rename to \"%1\" (same rules as ROM stems). "
                "Play is blocked for this user until fixed.")
                .arg(QString::fromStdString(game.expected_save_stem));
        }
        const auto map_key = game.username + '\n' + game.game_key;
        if (active_user_games.contains(map_key)) {
            row.active = true;
            if (const auto it = active_by_user_game.find(map_key); it != active_by_user_game.end()) {
                row.slot = it->second.slot_index;
                row.status_tip = QStringLiteral("Live session (slot %1): %2")
                    .arg(it->second.slot_index)
                    .arg(resolve_active_display_name(it->second, saves_hints_));
            } else {
                row.status_tip = QStringLiteral("Live host session on this save.");
            }
        }
        if (row.save_stem_mismatch) {
            row.status_tip = QStringLiteral(
                "Save basename does not match catalog stem \"%1\". "
                "Rename the file; play is blocked for this user.")
                .arg(row.expected_save_stem);
        }
        pending.push_back(std::move(row));
    }

    for (const auto& active : unmatched_actives) {
        if (!user.empty() && active.username != user) {
            continue;
        }
        if (!system.empty() && !active.system_key.empty()
            && normalize_save_browser_system_key(active.system_key)
                != normalize_save_browser_system_key(system)) {
            continue;
        }
        const auto username_q = QString::fromStdString(active.username);
        const auto system_key_norm = normalize_save_browser_system_key(active.system_key);
        const auto system_label_q = QString::fromStdString(
            system_key_norm.empty() ? "Unknown" : save_system_label(system_key_norm));
        const auto display = resolve_active_display_name(active, saves_hints_);
        const auto game_label = QStringLiteral("Playing: %1").arg(display);
        if (!passes_filter(username_q, system_label_q, game_label)) {
            continue;
        }
        PendingGameRow row;
        row.username = username_q;
        row.system_key = QString::fromStdString(system_key_norm);
        row.system_label = system_label_q;
        row.display_name = game_label;
        row.size_text = QStringLiteral("—");
        row.path_tip = QStringLiteral(
            "Live session with no matching save row (common for PS2 memcards). "
            "Slot %1 · cadence game %2")
            .arg(active.slot_index)
            .arg(QString::fromStdString(active.game_id));
        row.active = true;
        row.synthetic = true;
        row.slot = active.slot_index;
        row.catalog_game_id = QString::fromStdString(active.game_id);
        row.status_tip = QStringLiteral("Live host session (slot %1).").arg(active.slot_index);
        pending.push_back(std::move(row));
    }

    // Ensure Connected users appear even with no matching save rows.
    std::unordered_set<std::string> users_in_pending;
    for (const auto& row : pending) {
        users_in_pending.insert(row.username.toStdString());
    }
    for (const auto& client : connected_clients) {
        if (active_usernames.contains(client.username)) {
            continue;
        }
        if (!user.empty() && client.username != user) {
            continue;
        }
        if (users_in_pending.contains(client.username)) {
            continue;
        }
        const auto username_q = QString::fromStdString(client.username);
        if (!filter.isEmpty() && !username_q.toLower().contains(filter)) {
            continue;
        }
        // Placeholder so the user node is created; no game children.
        PendingGameRow row;
        row.username = username_q;
        pending.push_back(std::move(row));
        users_in_pending.insert(client.username);
    }

    // Group by user/system for tree construction; header sort reorders siblings after.
    std::sort(pending.begin(), pending.end(), [](const PendingGameRow& a, const PendingGameRow& b) {
        if (a.username != b.username) {
            return a.username.toLower() < b.username.toLower();
        }
        if (a.system_label != b.system_label) {
            return a.system_label.toLower() < b.system_label.toLower();
        }
        return a.display_name.toLower() < b.display_name.toLower();
    });

    const int sort_column = saves_tree_->sortColumn();
    const auto sort_order = saves_tree_->header()->sortIndicatorOrder();
    saves_tree_->setSortingEnabled(false);
    saves_tree_->clear();
    int game_shown = 0;
    int active_shown = 0;
    int connected_shown = 0;

    SavesTreeItem* user_item = nullptr;
    SavesTreeItem* system_item = nullptr;
    QString current_user;
    QString current_system_key;
    std::unordered_set<std::string> users_marked_connected;

    auto ensure_user = [&](const QString& username) -> SavesTreeItem* {
        if (user_item != nullptr && current_user == username) {
            return user_item;
        }
        user_item = new SavesTreeItem(saves_tree_);
        user_item->setText(0, username);
        apply_saves_node_roles(user_item, SavesNodeKind::User, username);
        auto font = user_item->font(0);
        font.setBold(true);
        user_item->setFont(0, font);

        if (connected_usernames.contains(username.toStdString())
            && !users_marked_connected.contains(username.toStdString())) {
            user_item->setText(2, QStringLiteral("Connected"));
            QString tip = QStringLiteral("Authenticated control connection (not playing).");
            if (const auto it = connected_by_user.find(username.toStdString());
                it != connected_by_user.end()) {
                user_item->setData(0, kUserRoleSlot, it->second.slot_index);
                tip = QStringLiteral("Connected as client %1 (%2)")
                    .arg(it->second.client_id)
                    .arg(QString::fromStdString(
                        it->second.phase.empty() ? "session" : it->second.phase));
            }
            user_item->setToolTip(2, tip);
            users_marked_connected.insert(username.toStdString());
            ++connected_shown;
        }

        const bool expand_user = previously_expanded.contains(username)
            || connected_usernames.contains(username.toStdString())
            || active_usernames.contains(username.toStdString())
            || !filter.isEmpty();
        user_item->setExpanded(expand_user);
        current_user = username;
        current_system_key.clear();
        system_item = nullptr;
        return user_item;
    };

    auto ensure_system = [&](const QString& username,
                             const QString& system_key,
                             const QString& system_label) -> SavesTreeItem* {
        auto* parent = ensure_user(username);
        if (system_item != nullptr && current_system_key == system_key && !system_key.isEmpty()) {
            return system_item;
        }
        // Empty system_key + empty label means "user only" placeholder — no system node.
        if (system_key.isEmpty() && system_label.isEmpty()) {
            system_item = nullptr;
            current_system_key.clear();
            return nullptr;
        }
        system_item = new SavesTreeItem(parent);
        system_item->setText(0, system_label.isEmpty() ? QStringLiteral("Unknown") : system_label);
        apply_saves_node_roles(system_item, SavesNodeKind::System, username, system_key);
        const auto expand_key = username + QLatin1Char('\n') + system_key;
        system_item->setExpanded(
            previously_expanded.contains(expand_key) || !filter.isEmpty()
            || active_usernames.contains(username.toStdString()));
        current_system_key = system_key;
        return system_item;
    };

    for (const auto& row : pending) {
        // Connected-only placeholder: user node, no children.
        if (row.display_name.isEmpty() && row.system_key.isEmpty() && !row.active) {
            ensure_user(row.username);
            continue;
        }

        auto* parent = ensure_system(row.username, row.system_key, row.system_label);
        if (parent == nullptr) {
            continue;
        }
        auto* game_item = new SavesTreeItem(parent);
        game_item->setText(0, row.display_name);
        game_item->setText(1, row.size_text);
        game_item->setData(1, kUserRoleSortBytes, QVariant::fromValue(row.bytes));
        apply_saves_node_roles(
            game_item,
            SavesNodeKind::Game,
            row.username,
            row.system_key,
            row.game_key,
            row.synthetic,
            row.slot);
        if (!row.catalog_game_id.isEmpty()) {
            game_item->setData(0, kUserRoleCatalogGameId, row.catalog_game_id);
        }
        if (!row.path_tip.isEmpty()) {
            game_item->setToolTip(0, row.path_tip);
        }
        if (row.save_stem_mismatch) {
            const auto warn = row.expected_save_stem.isEmpty()
                ? QStringLiteral("bad save name")
                : QStringLiteral("bad save name → %1").arg(row.expected_save_stem);
            game_item->setText(0, QStringLiteral("%1  (%2)").arg(row.display_name, warn));
            game_item->setText(2, QStringLiteral("Bad name"));
            game_item->setToolTip(2, row.status_tip);
            game_item->setForeground(0, QBrush(QColor(160, 90, 20)));
            game_item->setForeground(2, QBrush(QColor(160, 90, 20)));
        } else if (row.active) {
            game_item->setText(2, QStringLiteral("Active"));
            game_item->setToolTip(2, row.status_tip);
            ++active_shown;
        }
        ++game_shown;
    }

    saves_tree_->sortByColumn(sort_column, sort_order);
    saves_tree_->setSortingEnabled(true);
    {
        const QSignalBlocker block(saves_tree_);
        restore_selected_saves_identity(saves_tree_, previously_selected);
        if (saves_tree_->verticalScrollBar() != nullptr) {
            saves_tree_->verticalScrollBar()->setValue(previous_vscroll);
        }
        if (saves_tree_->horizontalScrollBar() != nullptr) {
            saves_tree_->horizontalScrollBar()->setValue(previous_hscroll);
        }
    }
    QString status = QStringLiteral("%1 user(s), %2 game(s) under %3")
        .arg(saves_tree_->topLevelItemCount())
        .arg(game_shown)
        .arg(QString::fromStdString(root.string()));
    if (active_shown > 0 || connected_shown > 0) {
        status += QStringLiteral(" — %1 active, %2 connected")
            .arg(active_shown)
            .arg(connected_shown);
    }
    if (ps2_prewarm_running_.load()) {
        status += QStringLiteral(" — reading PS2 memory cards…");
    }
    saves_status_->setText(status);
    update_saves_action_enabled();
    refresh_saves_blocked_list();
}

void MainWindow::update_saves_action_enabled() {
    const auto* item = saves_tree_ != nullptr ? saves_tree_->currentItem() : nullptr;
    const bool has_row = item != nullptr;
    const int kind = has_row ? item->data(0, kUserRoleNodeKind).toInt() : -1;
    const bool synthetic = has_row && item->data(0, kUserRoleSynthetic).toBool();
    const bool has_user_filter = saves_user_ != nullptr && !saves_user_->currentData().toString().isEmpty();
    const bool has_system_filter =
        saves_system_ != nullptr && !saves_system_->currentData().toString().isEmpty();
    const bool has_user = has_row || has_user_filter;
    const bool has_system =
        (has_row
         && (kind == static_cast<int>(SavesNodeKind::System)
             || kind == static_cast<int>(SavesNodeKind::Game))
         && !item->data(0, kUserRoleSystemKey).toString().isEmpty())
        || (has_user_filter && has_system_filter);
    const bool has_game = has_row && kind == static_cast<int>(SavesNodeKind::Game) && !synthetic
        && !item->data(0, kUserRoleGameKey).toString().isEmpty();

    if (saves_remove_user_action_ != nullptr) {
        saves_remove_user_action_->setEnabled(has_user);
    }
    if (saves_remove_system_action_ != nullptr) {
        saves_remove_system_action_->setEnabled(has_system);
    }
    if (saves_remove_game_action_ != nullptr) {
        saves_remove_game_action_->setEnabled(has_game);
    }
    if (saves_kick_action_ != nullptr) {
        saves_kick_action_->setEnabled(true);
    }
    if (saves_block_game_action_ != nullptr) {
        saves_block_game_action_->setEnabled(saves_selected_catalog_game_id().has_value());
    }
    if (saves_unblock_game_action_ != nullptr) {
        const bool has_blocked =
            saves_blocked_list_ != nullptr && saves_blocked_list_->currentItem() != nullptr;
        saves_unblock_game_action_->setEnabled(has_blocked);
    }
}

bool MainWindow::confirm_saves_destructive(const QString& title, const QString& detail) {
    if (saves_host_busy()) {
        const auto answer = QMessageBox::warning(
            this,
            title,
            detail
                + "\n\nHost Runner is currently running. Deleting saves for an active "
                  "player can corrupt that session. Prefer Kick for live players, "
                  "or Stop Host first unless you are sure.",
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
        append_log(host_log_, QStringLiteral("[users] created %1").arg(name));
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
        username = item->data(0, kUserRoleUsername).toString();
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
            QStringLiteral("Permanently delete all save data for “%1”?").arg(username))) {
        return;
    }
    try {
        delete_save_user(save_root_path(), username.toStdString());
        saves_status_->setText(QStringLiteral("Deleted user “%1”.").arg(username));
        append_log(host_log_, QStringLiteral("[users] deleted user %1").arg(username));
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
        username = item->data(0, kUserRoleUsername).toString();
        system_key = item->data(0, kUserRoleSystemKey).toString();
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
            "Select a row (or user + system filters) first.");
        return;
    }
    if (!confirm_saves_destructive(
            "Remove System",
            QStringLiteral("Delete all “%1” saves for “%2”?")
                .arg(system_label, username))) {
        return;
    }
    try {
        const auto removed = delete_save_system(
            save_root_path(),
            username.toStdString(),
            system_key.toStdString(),
            saves_hints_);
        saves_status_->setText(
            QStringLiteral("Removed %1 “%2” save(s) for %3.")
                .arg(removed)
                .arg(system_label, username));
        append_log(
            host_log_,
            QStringLiteral("[users] removed system %1 for %2 (%3)")
                .arg(system_key, username)
                .arg(removed));
        refresh_saves_browser();
    } catch (const std::exception& error) {
        QMessageBox::warning(this, "Remove System", error.what());
    }
}

void MainWindow::saves_remove_game() {
    const auto* item = saves_tree_->currentItem();
    if (item == nullptr || item->data(0, kUserRoleSynthetic).toBool()) {
        QMessageBox::information(this, "Remove Game", "Select a saved game row first.");
        return;
    }
    const auto username = item->data(0, kUserRoleUsername).toString();
    const auto game_key = item->data(0, kUserRoleGameKey).toString();
    // Name is column 0; column 2 is Status (Active / Bad name) and is often empty.
    auto label = item->text(0).trimmed();
    const auto bad_marker = QStringLiteral("  (bad save name");
    const int bad_at = label.indexOf(bad_marker);
    if (bad_at >= 0) {
        label = label.left(bad_at).trimmed();
    }
    if (label.isEmpty()) {
        label = item->data(0, kUserRoleGameKey).toString();
    }
    if (username.isEmpty() || game_key.isEmpty()) {
        QMessageBox::information(this, "Remove Game", "Select a saved game row first.");
        return;
    }
    if (!confirm_saves_destructive(
            "Remove Game",
            QStringLiteral("Delete “%1” for “%2”?").arg(label, username))) {
        return;
    }
    try {
        delete_save_game(save_root_path(), username.toStdString(), game_key.toStdString());
        saves_status_->setText(QStringLiteral("Deleted “%1” for %2.").arg(label, username));
        append_log(
            host_log_,
            QStringLiteral("[users] deleted %1 for %2").arg(game_key, username));
        refresh_saves_browser();
    } catch (const std::exception& error) {
        QMessageBox::warning(this, "Remove Game", error.what());
    }
}

void MainWindow::saves_kick_user() {
    const auto root = save_root_path();
    const auto actives = load_users_active_sessions(root);
    const auto connected = list_connected_clients(root);

    enum class KickKind { ActiveSlot, ConnectedClient };
    struct KickTarget {
        KickKind kind = KickKind::ActiveSlot;
        ActiveSaveSession active;
        ConnectedClientPresence client;
        QString label;
    };
    std::vector<KickTarget> targets;
    QStringList labels;

    for (const auto& active : actives) {
        KickTarget target;
        target.kind = KickKind::ActiveSlot;
        target.active = active;
        const auto display = resolve_active_display_name(active, saves_hints_);
        target.label = QStringLiteral("Active — %1 — %2 (slot %3)")
            .arg(QString::fromStdString(active.username), display)
            .arg(active.slot_index);
        labels << target.label;
        targets.push_back(std::move(target));
    }

    for (const auto& client : connected) {
        // Seated save-owner of an Active slot is kicked via Active (ends the slot).
        bool covered_by_active = false;
        for (const auto& active : actives) {
            if (active.slot_index == client.slot_index
                && active.username == client.username
                && client.seated) {
                covered_by_active = true;
                break;
            }
        }
        if (covered_by_active) {
            continue;
        }
        KickTarget target;
        target.kind = KickKind::ConnectedClient;
        target.client = client;
        const auto phase = client.phase.empty()
            ? (client.slot_index < 0 ? QStringLiteral("lobby") : QStringLiteral("session"))
            : QString::fromStdString(client.phase);
        target.label = QStringLiteral("Connected — %1 (client %2, %3)")
            .arg(QString::fromStdString(client.username))
            .arg(client.client_id)
            .arg(phase);
        labels << target.label;
        targets.push_back(std::move(target));
    }

    if (targets.empty()) {
        QMessageBox::information(
            this,
            "Kick",
            "No Active sessions or Connected clients to kick.");
        return;
    }

    bool ok = false;
    const auto chosen = QInputDialog::getItem(
        this,
        "Kick",
        "Select an Active session or Connected client:",
        labels,
        0,
        false,
        &ok);
    if (!ok || chosen.isEmpty()) {
        return;
    }
    const int index = labels.indexOf(chosen);
    if (index < 0 || index >= static_cast<int>(targets.size())) {
        return;
    }
    const auto& target = targets[static_cast<std::size_t>(index)];

    if (target.kind == KickKind::ActiveSlot) {
        if (target.active.slot_index < 0) {
            QMessageBox::warning(this, "Kick", "Selected session has no slot index.");
            return;
        }
        if (QMessageBox::question(
                this,
                "Kick",
                QStringLiteral("Kick Active session for “%1” (slot %2)?\n\n"
                               "This requests normal session teardown for that slot.")
                    .arg(QString::fromStdString(target.active.username))
                    .arg(target.active.slot_index),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No)
            != QMessageBox::Yes) {
            return;
        }
        request_active_session_stop(root, target.active.slot_index, "kicked");
        saves_status_->setText(
            QStringLiteral("Kick requested for Active %1 (slot %2).")
                .arg(QString::fromStdString(target.active.username))
                .arg(target.active.slot_index));
        append_log(
            host_log_,
            QStringLiteral("[users] kick active %1 slot %2")
                .arg(QString::fromStdString(target.active.username))
                .arg(target.active.slot_index));
    } else {
        if (QMessageBox::question(
                this,
                "Kick",
                QStringLiteral("Disconnect “%1” (client %2)?\n\n"
                               "This closes their control connection only — not a blacklist. "
                               "If they are a seated primary, existing host rules may end "
                               "the shared session.")
                    .arg(QString::fromStdString(target.client.username))
                    .arg(target.client.client_id),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No)
            != QMessageBox::Yes) {
            return;
        }
        request_connected_client_disconnect(
            root,
            target.client.client_id,
            target.client.slot_index,
            "kicked");
        saves_status_->setText(
            QStringLiteral("Disconnect requested for %1 (client %2).")
                .arg(QString::fromStdString(target.client.username))
                .arg(target.client.client_id));
        append_log(
            host_log_,
            QStringLiteral("[users] disconnect connected %1 client %2")
                .arg(QString::fromStdString(target.client.username))
                .arg(target.client.client_id));
    }
    QTimer::singleShot(800, this, [this] { refresh_saves_browser_list(); });
}

QString MainWindow::saves_selected_username() const {
    if (saves_tree_ != nullptr) {
        if (const auto* item = saves_tree_->currentItem()) {
            const auto username = item->data(0, kUserRoleUsername).toString().trimmed();
            if (!username.isEmpty()) {
                return username;
            }
        }
    }
    if (saves_user_ != nullptr) {
        return saves_user_->currentData().toString().trimmed();
    }
    return {};
}

std::optional<std::string> MainWindow::saves_selected_catalog_game_id() const {
    if (saves_tree_ == nullptr) {
        return std::nullopt;
    }
    const auto* item = saves_tree_->currentItem();
    if (item == nullptr) {
        return std::nullopt;
    }
    if (item->data(0, kUserRoleNodeKind).toInt() != static_cast<int>(SavesNodeKind::Game)) {
        return std::nullopt;
    }
    const auto id = item->data(0, kUserRoleCatalogGameId).toString().trimmed();
    if (id.isEmpty()) {
        return std::nullopt;
    }
    return id.toStdString();
}

void MainWindow::refresh_saves_blocked_list() {
    if (saves_blocked_list_ == nullptr) {
        return;
    }
    const auto username = saves_selected_username();
    const QString previous_id =
        saves_blocked_list_->currentItem() != nullptr
            ? saves_blocked_list_->currentItem()->data(kBlockedRoleGameId).toString()
            : QString();

    const QSignalBlocker block(saves_blocked_list_);
    saves_blocked_list_->clear();
    if (saves_blocked_label_ != nullptr) {
        if (username.isEmpty()) {
            saves_blocked_label_->setText(QStringLiteral("Blocked games"));
        } else {
            saves_blocked_label_->setText(
                QStringLiteral("Blocked games for %1").arg(username));
        }
    }
    if (username.isEmpty()) {
        update_saves_action_enabled();
        return;
    }

    try {
        GameMetaStore meta;
        if (!meta.ready()) {
            update_saves_action_enabled();
            return;
        }
        QListWidgetItem* restore = nullptr;
        for (const auto& row : meta.list_user_game_blocks(username.toStdString())) {
            const auto title = row.display_name.empty() ? row.game_id : row.display_name;
            auto* item = new QListWidgetItem(QString::fromStdString(title));
            item->setData(kBlockedRoleGameId, QString::fromStdString(row.game_id));
            item->setToolTip(QString::fromStdString(row.game_id));
            saves_blocked_list_->addItem(item);
            if (!previous_id.isEmpty() && previous_id.toStdString() == row.game_id) {
                restore = item;
            }
        }
        if (restore != nullptr) {
            saves_blocked_list_->setCurrentItem(restore);
        }
    } catch (...) {
    }
    update_saves_action_enabled();
}

void MainWindow::saves_block_selected_game() {
    const auto username = saves_selected_username();
    const auto game_id = saves_selected_catalog_game_id();
    if (username.isEmpty() || !game_id.has_value()) {
        QMessageBox::information(
            this,
            "Block game",
            "Select a catalog-backed game row for a user first.\n\n"
            "Blocked titles are omitted from that user's game list after they connect.");
        return;
    }

    QString system_key;
    QString label;
    if (const auto* item = saves_tree_->currentItem()) {
        system_key = item->data(0, kUserRoleSystemKey).toString();
        label = item->text(0);
    }
    if (label.isEmpty()) {
        label = QString::fromStdString(*game_id);
    }

    if (QMessageBox::question(
            this,
            "Block game",
            QStringLiteral("Hide “%1” from %2's game list?\n\n"
                           "They will not see this title after connecting "
                           "(reconnect to apply if already connected).")
                .arg(label, username),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No)
        != QMessageBox::Yes) {
        return;
    }

    try {
        GameMetaStore meta;
        if (!meta.ready()) {
            QMessageBox::warning(this, "Block game", "Game meta database is not available.");
            return;
        }
        if (!meta.block_user_game(
                username.toStdString(),
                *game_id,
                system_key.toStdString())) {
            QMessageBox::warning(this, "Block game", "Could not write the block.");
            return;
        }
        saves_status_->setText(
            QStringLiteral("Blocked “%1” for %2 (hidden from their catalog).")
                .arg(label, username));
        append_log(
            host_log_,
            QStringLiteral("[users] block game %1 for %2")
                .arg(QString::fromStdString(*game_id), username));
        refresh_saves_blocked_list();
    } catch (const std::exception& error) {
        QMessageBox::warning(this, "Block game", error.what());
    }
}

void MainWindow::saves_unblock_selected_blocked_game() {
    const auto username = saves_selected_username();
    if (username.isEmpty() || saves_blocked_list_ == nullptr) {
        QMessageBox::information(this, "Unblock game", "Select a user and a blocked game first.");
        return;
    }
    const auto* item = saves_blocked_list_->currentItem();
    if (item == nullptr) {
        QMessageBox::information(this, "Unblock game", "Select a blocked game first.");
        return;
    }
    const auto game_id = item->data(kBlockedRoleGameId).toString().trimmed();
    const auto label = item->text().trimmed();
    if (game_id.isEmpty()) {
        return;
    }

    if (QMessageBox::question(
            this,
            "Unblock game",
            QStringLiteral("Show “%1” again in %2's game list?")
                .arg(label.isEmpty() ? game_id : label, username),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No)
        != QMessageBox::Yes) {
        return;
    }

    try {
        GameMetaStore meta;
        if (!meta.ready()) {
            QMessageBox::warning(this, "Unblock game", "Game meta database is not available.");
            return;
        }
        if (!meta.unblock_user_game(username.toStdString(), game_id.toStdString())) {
            QMessageBox::warning(this, "Unblock game", "Could not remove the block.");
            return;
        }
        saves_status_->setText(
            QStringLiteral("Unblocked “%1” for %2.").arg(
                label.isEmpty() ? game_id : label,
                username));
        append_log(
            host_log_,
            QStringLiteral("[users] unblock game %1 for %2").arg(game_id, username));
        refresh_saves_blocked_list();
    } catch (const std::exception& error) {
        QMessageBox::warning(this, "Unblock game", error.what());
    }
}

#endif // ARCHSTREAMER_HAS_HOST

} // namespace archstreamer::gui
