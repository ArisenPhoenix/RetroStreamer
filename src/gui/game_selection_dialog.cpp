#include "game_selection_dialog.hpp"

#include "common/catalog_presenter.hpp"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

namespace archstreamer::gui {

GameSelectionDialog::GameSelectionDialog(
    const GameList& catalog,
    const std::optional<std::string>& current_id,
    QWidget* parent)
    : QDialog(parent),
      catalog_(catalog),
      selected_id_(current_id) {
    setWindowTitle("Choose a Game");
    resize(720, 520);

    auto* root = new QVBoxLayout(this);

    filter_ = new QLineEdit(this);
    filter_->setPlaceholderText("Search games...");
    root->addWidget(filter_);

    list_ = new QListWidget(this);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    root->addWidget(list_, 1);

    for (const auto& game : catalog_.games) {
        auto* item = new QListWidgetItem(QString::fromStdString(format_game_summary(game)), list_);
        item->setData(Qt::UserRole, QString::fromStdString(game.id));
        if (current_id.has_value() && game.id == *current_id) {
            item->setSelected(true);
            list_->setCurrentItem(item);
        }
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    root->addWidget(buttons);

    connect(filter_, &QLineEdit::textChanged, this, &GameSelectionDialog::applyFilter);
    connect(list_, &QListWidget::itemDoubleClicked, this, &GameSelectionDialog::acceptSelection);
    connect(buttons, &QDialogButtonBox::accepted, this, &GameSelectionDialog::acceptSelection);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    if (list_->currentItem() == nullptr && list_->count() > 0) {
        list_->setCurrentRow(0);
    }
}

std::optional<std::string> GameSelectionDialog::selectedGameId() const {
    return selected_id_;
}

void GameSelectionDialog::applyFilter(const QString& text) {
    const auto needle = text.trimmed().toLower();
    for (int row = 0; row < list_->count(); ++row) {
        auto* item = list_->item(row);
        const auto haystack = item->text().toLower();
        item->setHidden(!needle.isEmpty() && !haystack.contains(needle));
    }
}

void GameSelectionDialog::acceptSelection() {
    if (list_->currentItem() == nullptr) {
        return;
    }
    selected_id_ = list_->currentItem()->data(Qt::UserRole).toString().toStdString();
    accept();
}

} // namespace archstreamer::gui
