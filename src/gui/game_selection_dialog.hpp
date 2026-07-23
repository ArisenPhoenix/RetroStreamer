#pragma once

#include "client/game_filter.hpp"
#include "common/protocol.hpp"

#include <QDialog>
#include <filesystem>
#include <optional>
#include <string>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;

namespace archstreamer::gui {

class GameSelectionDialog final : public QDialog {
    Q_OBJECT

public:
    GameSelectionDialog(
        const GameList& catalog,
        const std::optional<std::string>& current_id,
        std::filesystem::path art_root,
        GameFilter session_filter,
        QWidget* parent = nullptr);

    std::optional<std::string> selectedGameId() const;

private:
    void refreshFilteredList();
    void applyTextFilter();
    void updatePreview();
    void acceptSelection();
    GameFilter combinedFilter() const;

    GameList catalog_;
    GameList visible_;
    GameFilter session_filter_;
    std::optional<std::string> selected_id_;
    std::filesystem::path art_root_;
    QComboBox* system_ = nullptr;
    QComboBox* language_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QListWidget* list_ = nullptr;
    QLabel* preview_image_ = nullptr;
    QLabel* preview_text_ = nullptr;
    QLabel* count_ = nullptr;
};

} // namespace archstreamer::gui
