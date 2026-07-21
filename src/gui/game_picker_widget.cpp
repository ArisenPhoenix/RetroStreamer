#include "game_picker_widget.hpp"
#include "game_selection_dialog.hpp"

#include "common/catalog_presenter.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

namespace archstreamer::gui {

GamePickerWidget::GamePickerWidget(QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    summary_ = new QLabel("No game selected", this);
    summary_->setWordWrap(true);
    layout->addWidget(summary_, 1);

    choose_ = new QPushButton("Choose Game...", this);
    layout->addWidget(choose_);

    connect(choose_, &QPushButton::clicked, this, &GamePickerWidget::openDialog);
}

void GamePickerWidget::setCatalog(const GameList& catalog) {
    catalog_ = catalog;
    if (selected_id_.has_value() && find_game_by_id(catalog_, *selected_id_) == nullptr) {
        selected_id_.reset();
    }
    updateSummary();
}

void GamePickerWidget::setSelectedGameId(const std::string& game_id) {
    selected_id_ = game_id;
    updateSummary();
}

void GamePickerWidget::clearSelection() {
    selected_id_.reset();
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

    GameSelectionDialog dialog(catalog_, selected_id_, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    selected_id_ = dialog.selectedGameId();
    updateSummary();
    emit selectionChanged();
}

void GamePickerWidget::updateSummary() {
    if (!selected_id_.has_value()) {
        summary_->setText("No game selected");
        return;
    }

    if (const auto* game = find_game_by_id(catalog_, *selected_id_); game != nullptr) {
        summary_->setText(QString::fromStdString(format_game_summary(*game)));
        return;
    }

    summary_->setText(QString("Selected game id: %1").arg(QString::fromStdString(*selected_id_)));
}

} // namespace archstreamer::gui
