#pragma once

#include "common/protocol.hpp"

#include <QWidget>
#include <optional>
#include <string>

class QLabel;
class QPushButton;

namespace archstreamer::gui {

class GamePickerWidget final : public QWidget {
    Q_OBJECT

public:
    explicit GamePickerWidget(QWidget* parent = nullptr);

    void setCatalog(const GameList& catalog);
    void setSelectedGameId(const std::string& game_id);
    void clearSelection();
    bool hasSelection() const;
    std::optional<std::string> selectedGameId() const;
    std::optional<GameInfo> selectedGame() const;

signals:
    void selectionChanged();

private:
    void openDialog();
    void updateSummary();

    GameList catalog_;
    std::optional<std::string> selected_id_;
    QLabel* summary_ = nullptr;
    QPushButton* choose_ = nullptr;
};

} // namespace archstreamer::gui
