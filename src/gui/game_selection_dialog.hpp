#pragma once

#include "common/protocol.hpp"

#include <QDialog>
#include <filesystem>
#include <optional>
#include <string>

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
        QWidget* parent = nullptr);

    std::optional<std::string> selectedGameId() const;

private:
    void applyFilter(const QString& text);
    void updatePreview();
    void acceptSelection();
    void rebuildList();

    GameList catalog_;
    std::optional<std::string> selected_id_;
    std::filesystem::path art_root_;
    QLineEdit* filter_ = nullptr;
    QListWidget* list_ = nullptr;
    QLabel* preview_image_ = nullptr;
    QLabel* preview_text_ = nullptr;
};

} // namespace archstreamer::gui
