#include "game_selection_dialog.hpp"

#include "common/catalog_presenter.hpp"
#include "common/game_assets.hpp"
#include "common/game_identity.hpp"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QSettings>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace archstreamer::gui {

namespace {

constexpr int kMaxRecentGames = 8;
constexpr int kThumbW = 48;
constexpr int kThumbH = 64;
constexpr int kItemTypeGame = QTreeWidgetItem::UserType + 1;
constexpr int kItemTypeGroup = QTreeWidgetItem::UserType + 2;

QPixmap load_game_art_pixmap(const std::filesystem::path& art_root, const GameInfo& game, QSize size) {
    LocalGameAssetProvider provider({}, art_root);
    const auto path = resolve_game_display_art(
        provider,
        game.asset_key,
        game.display_name,
        game.canonical_name);
    QPixmap pixmap(QString::fromStdString(path.string()));
    if (pixmap.isNull()) {
        const auto placeholder = default_placeholder_art_path(art_root);
        pixmap = QPixmap(QString::fromStdString(placeholder.string()));
    }
    if (pixmap.isNull()) {
        pixmap = QPixmap(size);
        pixmap.fill(Qt::darkGray);
    }
    return pixmap.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

void populate_filter_combo(QComboBox* combo, const std::vector<std::string>& values) {
    combo->clear();
    combo->addItem(QStringLiteral("(Any)"), QString());
    for (const auto& value : values) {
        combo->addItem(QString::fromStdString(value), QString::fromStdString(value));
    }
}

std::string system_group_name(const GameInfo& game) {
    if (!game.system_name.empty()) {
        return game.system_name;
    }
    if (!game.system_key.empty()) {
        return game.system_key;
    }
    return "Other";
}

bool game_matches_needle(const GameInfo& game, const QString& needle) {
    if (needle.isEmpty()) {
        return true;
    }
    const auto haystack = QString::fromStdString(
        catalog_label_for(game.display_name, game.version) + " " + format_game_summary(game) + " "
        + game.system_name + " " + game.system_key)
                              .toLower();
    return haystack.contains(needle);
}

} // namespace

GameSelectionDialog::GameSelectionDialog(
    const GameList& catalog,
    const std::optional<std::string>& current_id,
    std::filesystem::path art_root,
    GameFilter session_filter,
    QString recent_settings_key,
    QWidget* parent)
    : QDialog(parent),
      catalog_(catalog),
      session_filter_(std::move(session_filter)),
      selected_id_(current_id),
      art_root_(std::move(art_root)),
      recent_settings_key_(std::move(recent_settings_key)) {
    setWindowTitle("Choose a Game");
    resize(980, 580);

    auto* root = new QHBoxLayout(this);

    auto* filters = new QGroupBox("Filters", this);
    auto* filter_form = new QFormLayout(filters);
    system_ = new QComboBox(filters);
    language_ = new QComboBox(filters);
    filter_ = new QLineEdit(filters);
    filter_->setPlaceholderText("Search games...");
    count_ = new QLabel(filters);
    populate_filter_combo(system_, systems_for_games(catalog_));
    populate_filter_combo(language_, languages_for_games(catalog_));
    filter_form->addRow("System", system_);
    filter_form->addRow("Language", language_);
    filter_form->addRow("Search", filter_);
    filter_form->addRow("", count_);
    root->addWidget(filters);

    auto* right = new QVBoxLayout();
    auto* body = new QHBoxLayout();
    tree_ = new QTreeWidget(this);
    tree_->setHeaderHidden(true);
    tree_->setRootIsDecorated(true);
    tree_->setUniformRowHeights(false);
    tree_->setIconSize(QSize(kThumbW, kThumbH));
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setAnimated(true);
    tree_->setIndentation(16);
    tree_->setExpandsOnDoubleClick(false);
    if (auto* header = tree_->header(); header != nullptr) {
        header->setStretchLastSection(true);
    }
    body->addWidget(tree_, 3);

    auto* preview = new QWidget(this);
    auto* preview_layout = new QVBoxLayout(preview);
    preview_image_ = new QLabel(preview);
    preview_image_->setAlignment(Qt::AlignCenter);
    preview_image_->setMinimumSize(220, 300);
    preview_image_->setStyleSheet("background: #222; border-radius: 8px;");
    preview_text_ = new QLabel("Select a game", preview);
    preview_text_->setWordWrap(true);
    preview_text_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    preview_layout->addWidget(preview_image_);
    preview_layout->addWidget(preview_text_, 1);
    body->addWidget(preview, 2);
    right->addLayout(body, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    right->addWidget(buttons);
    root->addLayout(right, 1);

    refreshFilteredList();

    connect(system_, &QComboBox::currentIndexChanged, this, [this](int) {
        refreshFilteredList();
    });
    connect(language_, &QComboBox::currentIndexChanged, this, [this](int) {
        refreshFilteredList();
    });
    connect(filter_, &QLineEdit::textChanged, this, [this](const QString&) {
        applyTextFilter();
    });
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem*, QTreeWidgetItem*) {
        updatePreview();
    });
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        if (item != nullptr && item->type() == kItemTypeGame) {
            acceptSelection();
        }
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &GameSelectionDialog::acceptSelection);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

std::optional<std::string> GameSelectionDialog::selectedGameId() const {
    return selected_id_;
}

GameFilter GameSelectionDialog::combinedFilter() const {
    auto filter = session_filter_;
    const auto system = system_->currentData().toString().trimmed();
    if (!system.isEmpty()) {
        filter.system_name = system.toStdString();
    } else {
        filter.system_name.reset();
    }
    const auto language = language_->currentData().toString().trimmed();
    if (!language.isEmpty()) {
        filter.language = language.toStdString();
    } else {
        filter.language.reset();
    }
    return filter;
}

std::vector<std::string> GameSelectionDialog::loadRecentIds() const {
    if (recent_settings_key_.isEmpty()) {
        return {};
    }
    QSettings settings(QStringLiteral("ArchStreamer"), QStringLiteral("ArchStreamer"));
    const auto values = settings.value(recent_settings_key_).toStringList();
    std::vector<std::string> ids;
    ids.reserve(static_cast<std::size_t>(values.size()));
    for (const auto& value : values) {
        const auto trimmed = value.trimmed();
        if (!trimmed.isEmpty()) {
            ids.push_back(trimmed.toStdString());
        }
    }
    return ids;
}

void GameSelectionDialog::rememberRecentId(const std::string& game_id) {
    if (recent_settings_key_.isEmpty() || game_id.empty()) {
        return;
    }
    QSettings settings(QStringLiteral("ArchStreamer"), QStringLiteral("ArchStreamer"));
    QStringList values = settings.value(recent_settings_key_).toStringList();
    const QString id = QString::fromStdString(game_id);
    values.removeAll(id);
    values.prepend(id);
    while (values.size() > kMaxRecentGames) {
        values.removeLast();
    }
    settings.setValue(recent_settings_key_, values);
}

QTreeWidgetItem* GameSelectionDialog::addGameLeaf(
    QTreeWidgetItem* parent,
    const GameInfo& game,
    QTreeWidgetItem*& selected_item) {
    auto* item = new QTreeWidgetItem(parent, kItemTypeGame);
    item->setText(0, QString::fromStdString(catalog_label_for(game.display_name, game.version)));
    item->setData(0, Qt::UserRole, QString::fromStdString(game.id));
    item->setToolTip(0, QString::fromStdString(format_game_summary(game)));
    item->setIcon(0, QIcon(load_game_art_pixmap(art_root_, game, QSize(kThumbW, kThumbH))));
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    if (selected_id_.has_value() && game.id == *selected_id_) {
        selected_item = item;
    }
    return item;
}

void GameSelectionDialog::refreshFilteredList() {
    visible_ = filter_games(catalog_, combinedFilter());
    tree_->clear();

    std::unordered_map<std::string, const GameInfo*> by_id;
    by_id.reserve(visible_.games.size());
    for (const auto& game : visible_.games) {
        by_id.emplace(game.id, &game);
    }

    QTreeWidgetItem* selected_item = nullptr;
    QTreeWidgetItem* selected_system_group = nullptr;

    // Recents (order preserved from settings).
    const auto recent_ids = loadRecentIds();
    std::vector<const GameInfo*> recent_games;
    recent_games.reserve(recent_ids.size());
    for (const auto& id : recent_ids) {
        if (const auto it = by_id.find(id); it != by_id.end()) {
            recent_games.push_back(it->second);
        }
    }
    if (!recent_games.empty()) {
        auto* group = new QTreeWidgetItem(tree_, kItemTypeGroup);
        group->setText(
            0,
            QStringLiteral("Recents (%1)").arg(static_cast<int>(recent_games.size())));
        group->setFlags(Qt::ItemIsEnabled);
        group->setExpanded(true);
        for (const auto* game : recent_games) {
            addGameLeaf(group, *game, selected_item);
        }
    }

    // System groups A–Z.
    std::map<std::string, std::vector<const GameInfo*>> grouped;
    for (const auto& game : visible_.games) {
        grouped[system_group_name(game)].push_back(&game);
    }
    for (auto& [system_name, games] : grouped) {
        std::sort(games.begin(), games.end(), [](const GameInfo* a, const GameInfo* b) {
            return QString::fromStdString(catalog_label_for(a->display_name, a->version))
                       .compare(
                           QString::fromStdString(catalog_label_for(b->display_name, b->version)),
                           Qt::CaseInsensitive)
                < 0;
        });
        auto* group = new QTreeWidgetItem(tree_, kItemTypeGroup);
        group->setText(
            0,
            QStringLiteral("%1 (%2)")
                .arg(QString::fromStdString(system_name))
                .arg(static_cast<int>(games.size())));
        group->setFlags(Qt::ItemIsEnabled);
        for (const auto* game : games) {
            addGameLeaf(group, *game, selected_item);
            if (selected_id_.has_value() && game->id == *selected_id_) {
                selected_system_group = group;
            }
        }
        // Expand the system that contains the current selection; otherwise leave collapsed.
        group->setExpanded(selected_system_group == group);
    }

    if (selected_item != nullptr) {
        tree_->setCurrentItem(selected_item);
        tree_->scrollToItem(selected_item);
    } else {
        // Prefer first recent leaf, else first system leaf.
        for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
            auto* group = tree_->topLevelItem(i);
            if (group->childCount() > 0) {
                group->setExpanded(true);
                tree_->setCurrentItem(group->child(0));
                break;
            }
        }
    }

    count_->setText(QString("%1 / %2 games")
                        .arg(visible_.games.size())
                        .arg(catalog_.games.size()));
    applyTextFilter();
    updatePreview();
}

void GameSelectionDialog::applyTextFilter() {
    const auto needle = filter_->text().trimmed().toLower();

    for (int g = 0; g < tree_->topLevelItemCount(); ++g) {
        auto* group = tree_->topLevelItem(g);
        int visible_children = 0;
        for (int c = 0; c < group->childCount(); ++c) {
            auto* leaf = group->child(c);
            const auto game_id = leaf->data(0, Qt::UserRole).toString().toStdString();
            const auto* game = find_game_by_id(catalog_, game_id);
            const bool match = game != nullptr && game_matches_needle(*game, needle);
            leaf->setHidden(!match);
            if (match) {
                ++visible_children;
            }
        }
        const bool hide_group = visible_children == 0;
        group->setHidden(hide_group);
        if (!hide_group && !needle.isEmpty()) {
            // Mobile parity: auto-expand groups that still have matches under search.
            group->setExpanded(true);
        }
    }

    if (!needle.isEmpty()) {
        // Count unique matches from system groups only (skip Recents duplicates).
        std::unordered_set<std::string> unique;
        for (int g = 0; g < tree_->topLevelItemCount(); ++g) {
            auto* group = tree_->topLevelItem(g);
            if (group->isHidden() || group->text(0).startsWith(QStringLiteral("Recents"))) {
                continue;
            }
            for (int c = 0; c < group->childCount(); ++c) {
                auto* leaf = group->child(c);
                if (!leaf->isHidden()) {
                    unique.insert(leaf->data(0, Qt::UserRole).toString().toStdString());
                }
            }
        }
        count_->setText(QString("%1 shown (search) / %2 filtered / %3 total")
                            .arg(unique.size())
                            .arg(visible_.games.size())
                            .arg(catalog_.games.size()));
    } else {
        count_->setText(QString("%1 / %2 games")
                            .arg(visible_.games.size())
                            .arg(catalog_.games.size()));
    }

    // If current selection is hidden, move to first visible game leaf.
    auto* current = tree_->currentItem();
    if (current == nullptr || current->isHidden() || current->type() != kItemTypeGame ||
        (current->parent() != nullptr && current->parent()->isHidden())) {
        QTreeWidgetItem* first = nullptr;
        for (int g = 0; g < tree_->topLevelItemCount() && first == nullptr; ++g) {
            auto* group = tree_->topLevelItem(g);
            if (group->isHidden()) {
                continue;
            }
            for (int c = 0; c < group->childCount(); ++c) {
                auto* leaf = group->child(c);
                if (!leaf->isHidden()) {
                    first = leaf;
                    break;
                }
            }
        }
        if (first != nullptr) {
            tree_->setCurrentItem(first);
        }
    }
    updatePreview();
}

void GameSelectionDialog::updatePreview() {
    auto* item = tree_->currentItem();
    if (item == nullptr || item->type() != kItemTypeGame || item->isHidden()) {
        preview_image_->clear();
        preview_text_->setText("Select a game");
        return;
    }

    const auto game_id = item->data(0, Qt::UserRole).toString().toStdString();
    const auto* game = find_game_by_id(catalog_, game_id);
    if (game == nullptr) {
        preview_image_->clear();
        preview_text_->setText("Select a game");
        return;
    }

    preview_image_->setPixmap(load_game_art_pixmap(art_root_, *game, QSize(220, 300)));
    preview_text_->setText(QString::fromStdString(format_game_summary(*game)));
}

void GameSelectionDialog::acceptSelection() {
    auto* item = tree_->currentItem();
    if (item == nullptr || item->type() != kItemTypeGame || item->isHidden()) {
        return;
    }
    selected_id_ = item->data(0, Qt::UserRole).toString().toStdString();
    rememberRecentId(*selected_id_);
    accept();
}

} // namespace archstreamer::gui
