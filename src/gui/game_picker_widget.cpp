#include "game_picker_widget.hpp"
#include "game_selection_dialog.hpp"

#include "common/catalog_paths.hpp"
#include "common/catalog_presenter.hpp"
#include "common/game_assets.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>

namespace archstreamer::gui {

namespace {

QPixmap load_thumbnail(const std::filesystem::path& art_root, const GameInfo& game) {
    LocalGameAssetProvider provider({}, art_root);
    const auto path = resolve_game_display_art(
        provider,
        game.asset_key,
        game.display_name,
        game.canonical_name);
    QPixmap pixmap(QString::fromStdString(path.string()));
    if (pixmap.isNull()) {
        pixmap = QPixmap(QString::fromStdString(default_placeholder_art_path(art_root).string()));
    }
    if (pixmap.isNull()) {
        pixmap = QPixmap(72, 96);
        pixmap.fill(Qt::darkGray);
    }
    return pixmap.scaled(72, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

} // namespace

GamePickerWidget::GamePickerWidget(QWidget* parent)
    : QWidget(parent),
      art_root_(DefaultArtRoot) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    thumbnail_ = new QLabel(this);
    thumbnail_->setFixedSize(72, 96);
    thumbnail_->setAlignment(Qt::AlignCenter);
    thumbnail_->setStyleSheet("background: #222; border-radius: 4px;");
    layout->addWidget(thumbnail_);

    summary_ = new QLabel("No game selected", this);
    summary_->setWordWrap(true);
    layout->addWidget(summary_, 1);

    choose_ = new QPushButton("Choose Game...", this);
    layout->addWidget(choose_);

    connect(choose_, &QPushButton::clicked, this, &GamePickerWidget::openDialog);
    updateSummary();
}

void GamePickerWidget::setArtRoot(std::filesystem::path art_root) {
    art_root_ = std::move(art_root);
    updateSummary();
}

void GamePickerWidget::setCatalog(const GameList& catalog) {
    catalog_ = catalog;
    if (selected_id_.has_value() && find_game_by_id(catalog_, *selected_id_) == nullptr) {
        selected_id_.reset();
    }
    updateSummary();
}

void GamePickerWidget::setSessionFilter(GameFilter filter) {
    session_filter_ = std::move(filter);
}

void GamePickerWidget::setSelectedGameId(const std::string& game_id) {
    selected_id_ = game_id;
    updateSummary();
}

void GamePickerWidget::clearSelection() {
    selected_id_.reset();
    updateSummary();
}

void GamePickerWidget::refreshArtDisplay() {
    updateSummary();
}

bool GamePickerWidget::hasSelection() const {
    return selected_id_.has_value();
}

std::optional<std::string> GamePickerWidget::selectedGameId() const {
    return selected_id_;
}

std::optional<GameInfo> GamePickerWidget::selectedGame() const {
    if (!selected_id_.has_value()) {
        return std::nullopt;
    }
    if (const auto* game = find_game_by_id(catalog_, *selected_id_); game != nullptr) {
        return *game;
    }
    return std::nullopt;
}

void GamePickerWidget::openDialog() {
    if (catalog_.games.empty()) {
        summary_->setText("No games available");
        return;
    }

    GameSelectionDialog dialog(catalog_, selected_id_, art_root_, session_filter_, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    selected_id_ = dialog.selectedGameId();
    updateSummary();
    emit selectionChanged();
}

void GamePickerWidget::updateSummary() {
    if (!selected_id_.has_value()) {
        thumbnail_->clear();
        summary_->setText("No game selected");
        return;
    }

    if (const auto* game = find_game_by_id(catalog_, *selected_id_); game != nullptr) {
        thumbnail_->setPixmap(load_thumbnail(art_root_, *game));
        summary_->setText(QString::fromStdString(format_game_summary(*game)));
        return;
    }

    thumbnail_->clear();
    summary_->setText(QString("Selected game id: %1").arg(QString::fromStdString(*selected_id_)));
}

} // namespace archstreamer::gui
