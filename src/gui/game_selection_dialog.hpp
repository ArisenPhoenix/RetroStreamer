#pragma once

#include "client/game_filter.hpp"
#include "common/protocol.hpp"

#include <QDialog>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace archstreamer::gui {

/** Modal catalog browser — Recents + collapsible systems (Android Games tab parity). */
class GameSelectionDialog final : public QDialog {
    Q_OBJECT

public:
    /**
     * @param recent_settings_key QSettings key for recent game ids (e.g. "client/recent_game_ids").
     *        Empty disables the Recents group.
     */
    GameSelectionDialog(
        const GameList& catalog,
        const std::optional<std::string>& current_id,
        std::filesystem::path art_root,
        GameFilter session_filter,
        QString recent_settings_key = {},
        QWidget* parent = nullptr);

    std::optional<std::string> selectedGameId() const;

private:
    void refreshFilteredList();
    void applyTextFilter();
    void updatePreview();
    void acceptSelection();
    GameFilter combinedFilter() const;
    std::vector<std::string> loadRecentIds() const;
    void rememberRecentId(const std::string& game_id);
    QTreeWidgetItem* addGameLeaf(
        QTreeWidgetItem* parent,
        const GameInfo& game,
        QTreeWidgetItem*& selected_item);

    GameList catalog_;
    GameList visible_;
    GameFilter session_filter_;
    std::optional<std::string> selected_id_;
    std::filesystem::path art_root_;
    QString recent_settings_key_;
    QComboBox* system_ = nullptr;
    QComboBox* language_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QLabel* preview_image_ = nullptr;
    QLabel* preview_text_ = nullptr;
    QLabel* count_ = nullptr;
};

} // namespace archstreamer::gui
