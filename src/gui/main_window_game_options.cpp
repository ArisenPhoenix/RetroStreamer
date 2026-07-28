#include "main_window.hpp"

#include "gui_logging.hpp"
#include "gui_util.hpp"
#include "game_picker_widget.hpp"
#include "host_search_dialog.hpp"
#include "common/catalog_paths.hpp"
#include "common/catalog_presenter.hpp"
#include "common/addresses.hpp"
#include "common/discovery.hpp"
#include "common/game_assets.hpp"
#include "common/platform/paths.hpp"
#include "common/steam_art_import.hpp"
#include "client/client_media_playback.hpp"
#include "client/game_filter.hpp"
#include "client/audio_playback_device.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPixmapCache>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QFileInfo>
#include <QDir>
#include <QCoreApplication>

#include <chrono>
#include <exception>
#include <iostream>
#include <optional>
#include <thread>


namespace archstreamer::gui {

void MainWindow::refresh_game_options_ui() {
    const bool session_active = disc_control_ != nullptr && [&] {
        std::lock_guard lock(disc_control_->mutex);
        return disc_control_->session_active;
    }();
    std::vector<std::string> labels;
    if (session_active && disc_control_) {
        std::lock_guard lock(disc_control_->mutex);
        labels = disc_control_->disc_labels;
    }
    const bool active = session_active && labels.size() >= 2;
    const QString status_text = !session_active
        ? QStringLiteral("Join a multi-disc session (.m3u) to swap discs.")
        : (!active
               ? QStringLiteral("Active session has no multi-disc playlist.")
               : QString("Multi-disc session active (%1 discs). Swap when the game asks.")
                     .arg(labels.size()));

    if (game_options_status_ != nullptr && game_options_status_->text() != status_text) {
        game_options_status_->setText(status_text);
    }

    if (game_options_disc_ != nullptr) {
        // Avoid clear()/addItem every 500ms — on Windows that churns USER objects.
        QStringList wanted_labels;
        wanted_labels.reserve(static_cast<int>(labels.size()));
        for (std::size_t i = 0; i < labels.size(); ++i) {
            wanted_labels.push_back(
                QString("%1: %2").arg(i + 1).arg(QString::fromStdString(labels[i])));
        }
        bool list_matches = active && game_options_disc_->count() == wanted_labels.size();
        if (list_matches) {
            for (int i = 0; i < wanted_labels.size(); ++i) {
                if (game_options_disc_->itemText(i) != wanted_labels[i] ||
                    game_options_disc_->itemData(i).toInt() != i) {
                    list_matches = false;
                    break;
                }
            }
        } else if (!active && game_options_disc_->count() == 0) {
            list_matches = true;
        }
        if (!list_matches) {
            const QSignalBlocker blocker(game_options_disc_);
            const auto previous = game_options_disc_->currentData().toInt();
            game_options_disc_->clear();
            if (active) {
                for (int i = 0; i < wanted_labels.size(); ++i) {
                    game_options_disc_->addItem(wanted_labels[i], i);
                }
                const auto index = game_options_disc_->findData(previous);
                game_options_disc_->setCurrentIndex(index >= 0 ? index : 0);
            }
        }
        if (game_options_disc_->isEnabled() != active) {
            game_options_disc_->setEnabled(active);
        }
    }
    if (game_options_swap_ != nullptr && game_options_swap_->isEnabled() != active) {
        game_options_swap_->setEnabled(active);
    }
    if (game_options_prev_ != nullptr && game_options_prev_->isEnabled() != active) {
        game_options_prev_->setEnabled(active);
    }
    if (game_options_next_ != nullptr && game_options_next_->isEnabled() != active) {
        game_options_next_->setEnabled(active);
    }

    bool link_capable = false;
    std::string link_system;
    const bool link_session = link_control_ != nullptr && [&] {
        std::lock_guard lock(link_control_->mutex);
        link_capable = link_control_->link_capable;
        link_system = link_control_->system_key;
        return link_control_->session_active;
    }();
    const bool link_active = link_session && link_capable;
    const QString link_status = !link_session
        ? QStringLiteral(
              "Join a link-capable session (GBA / DS / Switch) to request a peer.")
        : (!link_capable
               ? QString("Active session (%1) does not support link yet.")
                     .arg(QString::fromStdString(
                         link_system.empty() ? "unknown system" : link_system))
               : QStringLiteral(
                     "Enter a seated peer's username. Both must request each other. "
                     "Multi-instance link backends are not started yet."));
    // Don't overwrite a live host response message every poll tick.
    if (game_options_link_status_ != nullptr) {
        const auto current = game_options_link_status_->text();
        const bool is_live_response =
            current.startsWith(QStringLiteral("Link:")) ||
            current.startsWith(QStringLiteral("Link failed:"));
        if (!is_live_response && current != link_status) {
            game_options_link_status_->setText(link_status);
        } else if (!link_session && current != link_status) {
            game_options_link_status_->setText(link_status);
        }
    }
    if (game_options_link_user_ != nullptr &&
        game_options_link_user_->isEnabled() != link_active) {
        game_options_link_user_->setEnabled(link_active);
    }
    if (game_options_link_request_ != nullptr &&
        game_options_link_request_->isEnabled() != link_active) {
        game_options_link_request_->setEnabled(link_active);
    }
    if (game_options_link_cancel_ != nullptr &&
        game_options_link_cancel_->isEnabled() != link_active) {
        game_options_link_cancel_->setEnabled(link_active);
    }
}

} // namespace archstreamer::gui
