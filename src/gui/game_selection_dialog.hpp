#pragma once

#include "common/protocol.hpp"

#include <QDialog>
#include <optional>
#include <string>

class QLineEdit;
class QListWidget;

namespace archstreamer::gui {

class GameSelectionDialog final : public QDialog {
    Q_OBJECT

public:
    GameSelectionDialog(const GameList& catalog, const std::optional<std::string>& current_id, QWidget* parent = nullptr);

    std::optional<std::string> selectedGameId() const;

private:
    void applyFilter(const QString& text);
    void acceptSelection();

    GameList catalog_;
    std::optional<std::string> selected_id_;
    QLineEdit* filter_ = nullptr;
    QListWidget* list_ = nullptr;
};

} // namespace archstreamer::gui
