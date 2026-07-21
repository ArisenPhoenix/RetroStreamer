#include "game_selection_dialog.hpp"

#include "common/catalog_presenter.hpp"
#include "common/game_assets.hpp"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QPixmap>
#include <QVBoxLayout>

namespace archstreamer::gui {

namespace {

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

} // namespace

GameSelectionDialog::GameSelectionDialog(
    const GameList& catalog,
    const std::optional<std::string>& current_id,
    std::filesystem::path art_root,
    QWidget* parent)
    : QDialog(parent),
      catalog_(catalog),
      selected_id_(current_id),
      art_root_(std::move(art_root)) {
    setWindowTitle("Choose a Game");
    resize(900, 560);

    auto* root = new QVBoxLayout(this);

    filter_ = new QLineEdit(this);
    filter_->setPlaceholderText("Search games...");
    root->addWidget(filter_);

    auto* body = new QHBoxLayout();
    list_ = new QListWidget(this);
    list_->setViewMode(QListView::IconMode);
    list_->setIconSize(QSize(120, 160));
    list_->setResizeMode(QListWidget::Adjust);
    list_->setMovement(QListWidget::Static);
    list_->setSpacing(12);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setWordWrap(true);
    body->addWidget(list_, 3);

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
    root->addLayout(body, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    root->addWidget(buttons);

    rebuildList();

    connect(filter_, &QLineEdit::textChanged, this, &GameSelectionDialog::applyFilter);
    connect(list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem*, QListWidgetItem*) {
        updatePreview();
    });
    connect(list_, &QListWidget::itemDoubleClicked, this, &GameSelectionDialog::acceptSelection);
    connect(buttons, &QDialogButtonBox::accepted, this, &GameSelectionDialog::acceptSelection);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

std::optional<std::string> GameSelectionDialog::selectedGameId() const {
    return selected_id_;
}

void GameSelectionDialog::rebuildList() {
    list_->clear();
    QListWidgetItem* selected_item = nullptr;
    for (const auto& game : catalog_.games) {
        auto* item = new QListWidgetItem(QString::fromStdString(game.display_name), list_);
        item->setData(Qt::UserRole, QString::fromStdString(game.id));
        item->setToolTip(QString::fromStdString(format_game_summary(game)));
        item->setIcon(QIcon(load_game_art_pixmap(art_root_, game, QSize(120, 160))));
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        if (selected_id_.has_value() && game.id == *selected_id_) {
            selected_item = item;
        }
    }
    if (selected_item != nullptr) {
        list_->setCurrentItem(selected_item);
    } else if (list_->count() > 0) {
        list_->setCurrentRow(0);
    }
    updatePreview();
}

void GameSelectionDialog::applyFilter(const QString& text) {
    const auto needle = text.trimmed().toLower();
    for (int row = 0; row < list_->count(); ++row) {
        auto* item = list_->item(row);
        const auto haystack = item->text().toLower() + " " + item->toolTip().toLower();
        item->setHidden(!needle.isEmpty() && !haystack.contains(needle));
    }
}

void GameSelectionDialog::updatePreview() {
    if (list_->currentItem() == nullptr) {
        preview_image_->clear();
        preview_text_->setText("Select a game");
        return;
    }

    const auto game_id = list_->currentItem()->data(Qt::UserRole).toString().toStdString();
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
    if (list_->currentItem() == nullptr) {
        return;
    }
    selected_id_ = list_->currentItem()->data(Qt::UserRole).toString().toStdString();
    accept();
}

} // namespace archstreamer::gui
