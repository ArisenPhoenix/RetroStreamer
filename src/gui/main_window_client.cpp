#include "main_window.hpp"

#include "gui_logging.hpp"
#include "gui_util.hpp"
#include "game_picker_widget.hpp"
#include "host_search_dialog.hpp"
#include "client_video_controller.hpp"
#include "common/catalog_paths.hpp"
#include "common/catalog_presenter.hpp"
#include "common/addresses.hpp"
#include "common/client_logs.hpp"
#include "common/discovery.hpp"
#include "common/game_assets.hpp"
#include "common/platform/default_platform.hpp"
#include "common/platform/paths.hpp"
#include "common/serialization.hpp"
#include "common/steam_art_import.hpp"
#include "client/client_media_playback.hpp"
#include "client/game_filter.hpp"
#include "client/audio_playback_device.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
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
#include <QGuiApplication>
#include <QScreen>

#include <chrono>
#include <algorithm>
#include <exception>
#include <iostream>
#include <optional>
#include <thread>


namespace archstreamer::gui {

void MainWindow::refresh_client_controllers() {
    client_controllers_->clear();
    try {
        const auto devices = client_app_.list_controllers();
        for (std::size_t index = 0; index < devices.size(); ++index) {
            const auto label = QString::fromStdString(
                std::to_string(index) + ": " + devices[index].name + " [" + devices[index].id + "]");
            client_controllers_->addItem(label);
        }
        append_log(client_log_, QString("Detected %1 controller(s).").arg(devices.size()));
    } catch (const std::exception& error) {
        append_log(client_log_, QString("Controller scan failed: %1").arg(error.what()), GuiLogLevel::Quiet);
    }
}

archstreamer::GameFilter MainWindow::client_filter_from_fields() const {
    archstreamer::GameFilter filter;
    filter.requested_players = static_cast<std::uint8_t>(client_players_->value());
    if (selected_mode(client_mode_) == archstreamer::GameSessionMode::Multiplayer) {
        filter.mode = archstreamer::GameFilterMode::Multiplayer;
    } else {
        filter.mode = archstreamer::GameFilterMode::SinglePlayer;
    }
    return filter;
}

void MainWindow::refresh_filtered_client_games() {
    if (!client_catalog_loaded_) {
        return;
    }
    const auto filter = client_filter_from_fields();
    // Prefer the picker's current choice over a stale client/lastGameId (Connect used to
    // force Kingdom Hearts back every time). Fall back to host selection for local runs.
    std::optional<std::string> previous;
    if (client_game_picker_->hasSelection()) {
        previous = client_game_picker_->selectedGameId();
    }

    client_game_picker_->setSessionFilter(filter);
    client_game_picker_->setCatalog(client_full_catalog_);

    auto try_select = [this](const std::string& game_id) -> bool {
        if (game_id.empty()) {
            return false;
        }
        if (archstreamer::find_game_by_id(client_full_catalog_, game_id) == nullptr) {
            return false;
        }
        client_game_picker_->setSelectedGameId(game_id);
        return true;
    };

    bool restored = false;
    if (previous.has_value()) {
        restored = try_select(*previous);
    }
    if (!restored && !persisted_client_game_id_.isEmpty()) {
        restored = try_select(persisted_client_game_id_.toStdString());
    }
#ifdef ARCHSTREAMER_HAS_HOST
    if (!restored && host_game_picker_ != nullptr && host_game_picker_->hasSelection()) {
        restored = try_select(*host_game_picker_->selectedGameId());
    }
    if (!restored && !persisted_host_game_id_.isEmpty()) {
        restored = try_select(persisted_host_game_id_.toStdString());
    }
#endif
    if (restored && client_game_picker_->hasSelection()) {
        persisted_client_game_id_ =
            QString::fromStdString(*client_game_picker_->selectedGameId());
    }

    const auto filtered = archstreamer::filter_games(client_full_catalog_, filter);
    client_catalog_status_->setText(QString("%1 game(s) from host, %2 match mode/players")
        .arg(client_full_catalog_.games.size())
        .arg(filtered.games.size()));
}

archstreamer::ClientAppConfig MainWindow::client_config_from_fields() const {
    archstreamer::ClientAppConfig config;
    config.host = client_host_->text().toStdString();
    config.control_port = static_cast<std::uint16_t>(client_port_->value());
    config.input_port = static_cast<std::uint16_t>(client_input_port_->value());
    config.username = profile_client_username();
    config.display_name = archstreamer::preferred_steam_or_username_display_name(
        config.username,
        steam_account_id_text());
    if (client_password_ != nullptr) {
        config.password = client_password_->text().toStdString();
    }
    config.role = selected_client_role(client_role_);
    config.session_mode = selected_mode(client_mode_);
    config.filter = client_filter_from_fields();
    config.wants_video = client_video_->isChecked();
    config.wants_audio = client_audio_->isChecked();
    config.send_keyboard = client_send_keyboard_ != nullptr && client_send_keyboard_->isChecked();
    config.synced_av = client_synced_av_ != nullptr && client_synced_av_->isChecked();
    config.wanted_tier = selected_stream_quality();
    config.wanted_size = selected_stream_size();
    config.show_framecount =
        settings_show_framecount_ != nullptr && settings_show_framecount_->isChecked();
    if (const auto* screen = QGuiApplication::primaryScreen()) {
        const auto geom = screen->geometry();
        config.display_layout =
            geom.width() >= geom.height()
                ? archstreamer::DisplayLayoutPreference::Landscape
                : archstreamer::DisplayLayoutPreference::Portrait;
    } else {
        config.display_layout = archstreamer::DisplayLayoutPreference::Landscape;
    }

    for (const auto* item : client_controllers_->selectedItems()) {
        config.controller_indexes.push_back(static_cast<std::size_t>(client_controllers_->row(item)));
    }
    return config;
}

archstreamer::MediaQualityTier MainWindow::selected_stream_quality() const {
    if (client_stream_quality_ == nullptr || client_stream_quality_->currentData().isNull()) {
        return archstreamer::MediaQualityTier::Auto;
    }
    return static_cast<archstreamer::MediaQualityTier>(client_stream_quality_->currentData().toInt());
}

archstreamer::MediaStreamSize MainWindow::selected_stream_size() const {
    if (client_stream_size_ == nullptr || client_stream_size_->currentData().isNull()) {
        return archstreamer::MediaStreamSize::Auto;
    }
    return static_cast<archstreamer::MediaStreamSize>(client_stream_size_->currentData().toInt());
}

void MainWindow::apply_client_host(const QString& address, int control_port, int input_port, const QString& label) {
    const auto changed =
        client_host_->text() != address ||
        client_port_->value() != control_port ||
        client_input_port_->value() != input_port ||
        client_host_label_ != label;
    client_host_->setText(address);
    client_port_->setValue(control_port);
    client_input_port_->setValue(input_port);
    client_host_label_ = label;
    update_client_host_summary(label);
    if (changed) {
        append_log(client_log_, QString("Selected host %1 (control %2, input %3)")
            .arg(address)
            .arg(control_port)
            .arg(input_port));
        persist_settings_if_idle();
    }
}

void MainWindow::update_client_host_summary(const QString& label) {
    if (client_host_summary_ == nullptr || client_host_ == nullptr) {
        return;
    }
    const auto address = client_host_->text().trimmed();
    if (address.isEmpty()) {
        client_host_summary_->setText("No host selected");
        return;
    }
    if (!label.isEmpty()) {
        client_host_summary_->setText(QString("%1 — %2:%3/%4")
            .arg(label, address)
            .arg(client_port_->value())
            .arg(client_input_port_->value()));
        return;
    }
    client_host_summary_->setText(QString("%1 (control %2, input %3)")
        .arg(address)
        .arg(client_port_->value())
        .arg(client_input_port_->value()));
}

void MainWindow::open_host_search_dialog() {
    archstreamer::gui::HostSearchDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const auto host = dialog.selectedHost();
    if (!host.has_value()) {
        return;
    }
    apply_client_host(
        QString::fromStdString(host->address),
        host->control_port,
        host->input_port,
        QString::fromStdString(host->username));
}

void MainWindow::start_client_host_auto_pick() {
    if (client_host_ == nullptr) {
        return;
    }
    stop_client_host_auto_pick();
    try {
        client_auto_browser_ = std::make_unique<archstreamer::HostDiscoveryBrowser>();
        std::vector<std::string> seeds;
        QSettings settings(QStringLiteral("ArchStreamer"), QStringLiteral("ArchStreamer"));
        const auto saved = settings.value(QStringLiteral("client/hostAddress")).toString().trimmed();
        const auto field = client_host_->text().trimmed();
        const auto seed_candidate = !field.isEmpty() ? field : saved;
        if (!seed_candidate.isEmpty() && seed_candidate != QStringLiteral("127.0.0.1") &&
            !seed_candidate.startsWith(QStringLiteral("127."))) {
            seeds.push_back(seed_candidate.toStdString());
        }
        client_auto_browser_->set_seed_hosts(std::move(seeds));
    } catch (const std::exception& error) {
        append_log(client_log_, QString("Host auto-pick unavailable: %1").arg(error.what()));
        return;
    }
    client_auto_pick_timer_ = new QTimer(this);
    client_auto_pick_timer_->setInterval(1000);
    client_auto_pick_attempts_ = 0;
    connect(client_auto_pick_timer_, &QTimer::timeout, this, [this] {
        if (client_host_ == nullptr || !client_auto_browser_) {
            stop_client_host_auto_pick();
            return;
        }
        try {
            client_auto_browser_->poll();
            client_auto_browser_->expire_older_than(std::chrono::seconds(8));
            const auto live = client_auto_browser_->hosts();
            const auto current = client_host_->text().trimmed().toStdString();
            const bool current_empty = current.empty();
            const bool current_live = std::any_of(
                live.begin(),
                live.end(),
                [&](const archstreamer::DiscoveredHost& host) { return host.address == current; });

            // Keep a reachable saved/current host; only fill or switch when it's missing.
            if (!current_empty && current_live) {
                stop_client_host_auto_pick();
                return;
            }

            if (const auto preferred = archstreamer::prefer_discovered_host(live);
                preferred.has_value()) {
                if (preferred->address != current) {
                    apply_client_host(
                        QString::fromStdString(preferred->address),
                        preferred->control_port,
                        preferred->input_port,
                        QString::fromStdString(preferred->username));
                    if (current_empty) {
                        append_log(client_log_, "Auto-selected LAN host (same-subnet preferred).");
                    } else {
                        append_log(
                            client_log_,
                            QString("Saved host unreachable — switched to %1 @ %2")
                                .arg(QString::fromStdString(preferred->username))
                                .arg(QString::fromStdString(preferred->address)));
                    }
                }
                stop_client_host_auto_pick();
                return;
            }
        } catch (const std::exception& error) {
            append_log(client_log_, QString("Host auto-pick error: %1").arg(error.what()));
            stop_client_host_auto_pick();
            return;
        }
        ++client_auto_pick_attempts_;
        if (client_auto_pick_attempts_ >= 8) {
            stop_client_host_auto_pick();
        }
    });
    client_auto_pick_timer_->start();
    append_log(client_log_, "Looking for a LAN host…");
}

void MainWindow::stop_client_host_auto_pick() {
    if (client_auto_pick_timer_ != nullptr) {
        client_auto_pick_timer_->stop();
        client_auto_pick_timer_->deleteLater();
        client_auto_pick_timer_ = nullptr;
    }
    client_auto_browser_.reset();
}

void MainWindow::connect_client() {
    if (client_host_ == nullptr || client_host_->text().trimmed().isEmpty()) {
        append_log(client_log_, "Select a host (Select Host… or This PC) before Connect.");
        return;
    }
    const auto host_text = client_host_->text().trimmed();
    if (host_text == QStringLiteral("127.0.0.1") || host_text.startsWith(QStringLiteral("127."))) {
        append_log(
            client_log_,
            "Host is This PC (127.0.0.1) — that only works if ArchStreamer Host is running on "
            "THIS Windows machine. For the Linux host, use Select Host… and pick the LAN entry "
            "(e.g. 192.168.x.x), not This PC.");
    }
    // Video-window close ends the session worker, but std::thread stays joinable until
    // joined. Auto-reap finished workers so Connect/Join work without Stop Client.
    if (client_thread_.joinable() && !client_session_live_.load()) {
        client_thread_.join();
    }
    if (client_thread_.joinable()) {
        append_log(client_log_, "Stop the running client session before reconnecting.");
        return;
    }
    if (client_connect_thread_.joinable()) {
        if (client_connecting_.load()) {
            append_log(client_log_, "Client catalog fetch is already running.");
            return;
        }
        client_connect_thread_.join();
    }

    auto config = client_config_from_fields();
    append_log(client_log_, QString("Connecting to %1:%2...")
        .arg(QString::fromStdString(config.host))
        .arg(config.control_port));
    client_catalog_status_->setText("Connecting...");
    client_connecting_ = true;

    client_connect_thread_ = std::thread([this, config = std::move(config)] {
        try {
            const auto catalog = client_app_.fetch_catalog(config);
            QMetaObject::invokeMethod(
                this,
                [this, full = std::move(catalog.full_catalog),
                 art_cache = std::move(catalog.art_cache_root)]() mutable {
                    client_full_catalog_ = std::move(full);
                    client_catalog_loaded_ = true;
                    if (!art_cache.empty()) {
                        client_game_picker_->setArtRoot(art_cache);
                        append_log(client_log_, QString("Using host art cache: %1")
                            .arg(QString::fromStdString(art_cache.string())));
                    }
                    refresh_filtered_client_games();
                    append_log(client_log_, QString("Connected: received %1 games.")
                        .arg(client_full_catalog_.games.size()));
                    const auto filtered = archstreamer::filter_games(
                        client_full_catalog_,
                        client_filter_from_fields());
                    if (!filtered.games.empty()) {
                        append_log(
                            client_log_,
                            QString("First game matching mode/players: %1")
                                .arg(QString::fromStdString(archstreamer::format_game_summary(filtered.games.front()))));
                    } else {
                        append_log(client_log_, "Catalog connected, but no games matched the current mode/players.");
                    }
                    append_log(
                        client_log_,
                        "Tip: match the host Mode and selected game before Join, or the lobby will reject the hello. "
                        "System/language filters are in Choose Game.");
                },
                Qt::QueuedConnection);
        } catch (const std::exception& error) {
            const auto message = QString::fromLocal8Bit(error.what());
            QMetaObject::invokeMethod(
                this,
                [this, message] {
                    append_log(client_log_, QString("Connect failed: %1").arg(message), GuiLogLevel::Quiet);
                    if (message.contains("failed to connect TCP socket")) {
                        append_log(client_log_, "No host is listening on that address/port.");
                        append_log(client_log_, "Start Host first and wait until the Host tab says it is running.");
                    }
                    client_catalog_status_->setText("Connect failed");
                },
                Qt::QueuedConnection);
        }
        client_connecting_ = false;
    });
}

void MainWindow::start_client() {
    if (client_thread_.joinable() && !client_session_live_.load()) {
        client_thread_.join();
    }
    if (client_thread_.joinable()) {
        append_log(client_log_, "Client session is already running.");
        return;
    }

    auto config = client_config_from_fields();
    if (client_connect_thread_.joinable()) {
        append_log(client_log_, "Waiting for catalog fetch to finish before joining.");
        client_connect_thread_.join();
    }
    if (!client_catalog_loaded_) {
        append_log(client_log_, "Connect and fetch the host catalog before joining.");
        return;
    }
    if (!client_game_picker_->hasSelection()) {
        append_log(client_log_, "Choose a game before joining.");
        return;
    }
    if (config.password.empty()) {
        const auto created = prompt_new_password("Create password");
        if (created.isEmpty()) {
            append_log(client_log_, "Password required before joining.");
            return;
        }
        if (client_password_ != nullptr) {
            client_password_->setText(created);
        }
        config.password = created.toStdString();
    }
    if (config.role == archstreamer::ClientParticipantRole::Player) {
        refresh_client_controllers();
        config = client_config_from_fields();
        if (config.filter.requested_players == 0) {
            append_log(client_log_, "Player role requires at least one local player seat.");
            return;
        }
        if (config.controller_indexes.empty() && client_controllers_->count() == 1 &&
            config.filter.requested_players == 1) {
            client_controllers_->item(0)->setSelected(true);
            config = client_config_from_fields();
            append_log(client_log_, "Auto-selected the only connected controller.");
        }
        if (config.controller_indexes.size() < config.filter.requested_players) {
            append_log(
                client_log_,
                QString("Select %1 controller(s) before joining as a player.")
                    .arg(config.filter.requested_players));
            return;
        }
        for (std::size_t index : config.controller_indexes) {
            if (static_cast<int>(index) < client_controllers_->count()) {
                append_log(
                    client_log_,
                    QString("Using controller: %1")
                        .arg(client_controllers_->item(static_cast<int>(index))->text()));
            }
        }
    } else if (!config.controller_indexes.empty()) {
        append_log(client_log_, "Viewer role ignores selected controllers.");
        config.controller_indexes.clear();
    }
    config.game_selector = *client_game_picker_->selectedGameId();
    append_log(
        client_log_,
        QString("Joining with mode=%1 players=%2 input_port=%3 game=%4")
            .arg(mode_name(config.session_mode))
            .arg(config.filter.requested_players)
            .arg(*config.input_port)
            .arg(QString::fromStdString(*config.game_selector)));

    client_stop_requested_ = false;
    client_session_live_ = true;
    disc_control_ = std::make_shared<archstreamer::DiscControlBridge>();
    link_control_ = std::make_shared<archstreamer::LinkControlBridge>();
    soft_keyboard_ = std::make_shared<archstreamer::SoftKeyboardBridge>();
    heartbeat_prefs_ = std::make_shared<archstreamer::ClientHeartbeatPrefs>();
    media_resync_ = std::make_shared<archstreamer::MediaResyncBridge>();
    if (client_resync_av_ != nullptr) {
        client_resync_av_->setEnabled(true);
    }
    {
        std::lock_guard lock(heartbeat_prefs_->mutex);
        heartbeat_prefs_->wanted_tier = config.wanted_tier;
        heartbeat_prefs_->wanted_size = config.wanted_size;
        heartbeat_prefs_->max_bitrate_kbps = config.max_bitrate_kbps;
        heartbeat_prefs_->show_framecount = config.show_framecount;
        heartbeat_prefs_->display_layout = config.display_layout;
    }

#ifndef _WIN32
    if (config.wants_video) {
        auto video_embed = std::make_shared<archstreamer::VideoEmbedBridge>();
        client_video_controller_ = std::make_unique<ClientVideoController>(this);
        client_video_controller_->setVideoEmbedBridge(video_embed);
        client_video_controller_->setHeartbeatPrefs(heartbeat_prefs_);
        QObject::connect(
            client_video_controller_.get(),
            &ClientVideoController::userClosed,
            this,
            [this] {
                append_log(client_log_, "Video window closed; stopping session.");
                client_stop_requested_ = true;
                // Do not join the session thread on the GUI/X11 thread — that is what
                // freezes the desktop. The worker QueuedConnection cleans up the surface.
            });
        QString system_name;
        QString game_name = QString::fromStdString(*config.game_selector);
        if (const auto game = client_game_picker_->selectedGame(); game.has_value()) {
            system_name = QString::fromStdString(game->system_name);
            if (!game->display_name.empty()) {
                game_name = QString::fromStdString(game->display_name);
            }
        }
        client_video_controller_->setTitleFromGame(system_name, game_name);
        client_video_controller_->setMode(ClientVideoMode::TopLevel);
        client_video_controller_->prepareForSession();
        config.video_embed_xid = client_video_controller_->embedXid();
        config.video_embed = std::move(video_embed);
        append_log(
            client_log_,
            QString("Video window ready (xid=%1).").arg(config.video_embed_xid));
    }
#endif

    client_thread_ = std::thread([this, config = std::move(config)]() mutable {
        try {
            auto connected_client_id = std::optional<archstreamer::ClientId>{};
            archstreamer::ClientAppCallbacks callbacks;
            callbacks.disc_control = disc_control_;
            callbacks.link_control = link_control_;
            callbacks.soft_keyboard = soft_keyboard_;
            callbacks.heartbeat_prefs = heartbeat_prefs_;
            callbacks.face_button_prefs = face_button_prefs_;
            callbacks.media_resync = media_resync_;
            callbacks.on_catalog = [this](const archstreamer::GameList& full, const archstreamer::GameList& filtered) {
                append_log(client_log_, QString("Received %1 games; %2 after filters.")
                    .arg(full.games.size())
                    .arg(filtered.games.size()));
            };
            callbacks.on_connected = [this, &connected_client_id](const archstreamer::ClientConnectionInfo& connection) {
                connected_client_id = connection.client_id;
                QMetaObject::invokeMethod(
                    client_catalog_status_,
                    [this] {
                        client_catalog_status_->setText("Joined session");
                    },
                    Qt::QueuedConnection);
                append_log(client_log_, QString("Connected as client %1, user %2.")
                    .arg(connection.client_id)
                    .arg(QString::fromStdString(connection.username)));
            };
            callbacks.on_seat_assignment = [this, &connected_client_id](const archstreamer::SeatAssignment& seats) {
                auto assigned = false;
                for (const auto& seat : seats.seats) {
                    if (!connected_client_id.has_value() || seat.client_id != *connected_client_id) {
                        continue;
                    }
                    assigned = true;
                    append_log(client_log_, QString("Client %1 local P%2 -> RetroArch P%3.")
                        .arg(seat.client_id)
                        .arg(seat.local_player + 1)
                        .arg(seat.retroarch_port + 1));
                }
                if (!assigned) {
                    append_log(client_log_, "Assigned as viewer.");
                }
            };
            callbacks.on_session_ready = [this](const archstreamer::SessionReady& ready) {
                append_log(client_log_, QString("Session ready: %1 player(s).").arg(ready.player_count));
            };
            callbacks.on_media_endpoint = [this](const archstreamer::MediaEndpoint& endpoint) {
                if (!endpoint.video_uri.empty()) {
                    append_log(client_log_, QString("Video: %1").arg(QString::fromStdString(endpoint.video_uri)));
                    append_log(client_log_, "Starting GStreamer video into ArchStreamer window.");
                } else if (client_video_->isChecked()) {
                    append_log(
                        client_log_,
                        "Requested video, but host did not provide a video endpoint "
                        "(host is not streaming video — enable Stream video on the Host tab).");
                }
                if (!endpoint.audio_uri.empty()) {
                    append_log(client_log_, QString("Audio: %1").arg(QString::fromStdString(endpoint.audio_uri)));
                    append_log(client_log_, "Starting GStreamer audio receiver.");
                } else if (client_audio_->isChecked()) {
                    append_log(client_log_, "Requested audio, but host did not provide an audio endpoint.");
                }
            };
            callbacks.on_session_starting = [this](const archstreamer::SessionStarting& starting) {
                append_log(client_log_, QString("Session starting: %1 player(s).").arg(starting.player_count));
            };
            callbacks.on_session_ended = [this](const std::string& reason) {
                append_log(client_log_, QString("Session ended: %1").arg(QString::fromStdString(reason)), GuiLogLevel::Quiet);
            };
            callbacks.on_host_disconnected = [this] {
                append_log(client_log_, "Host disconnected.", GuiLogLevel::Quiet);
            };
            callbacks.on_input_streaming_started = [this](const std::string& host, std::uint16_t port) {
                append_log(client_log_, QString("Streaming input to %1:%2.")
                    .arg(QString::fromStdString(host))
                    .arg(port));
                append_log(
                    client_log_,
                    "Session live — heartbeats/input run quietly until Stop or the host ends.");
            };
            callbacks.on_waiting_without_input = [this] {
                append_log(client_log_, "Waiting for session end (no input streaming).");
            };
            callbacks.on_status = [this](const std::string& message) {
                append_log(client_log_, QString::fromStdString(message));
            };
            callbacks.on_password_change_required = [this](const std::string& /*current*/) {
                QString new_password;
                QMetaObject::invokeMethod(
                    this,
                    [this, &new_password] {
                        new_password = prompt_new_password("Host requires a new password");
                        if (!new_password.isEmpty() && client_password_ != nullptr) {
                            client_password_->setText(new_password);
                        }
                    },
                    Qt::BlockingQueuedConnection);
                return new_password.toStdString();
            };

            client_app_.run_session(
                config,
                [this] {
                    return client_stop_requested_.load();
                },
                callbacks);
        } catch (const std::exception& error) {
            const auto message = QString::fromLocal8Bit(error.what());
            append_log(client_log_, QString("Client error: %1").arg(message), GuiLogLevel::Quiet);
            if (message.contains("selected different games") || message.contains("selected different session modes")) {
                append_log(client_log_, "Host already locked game/mode. Match the Host tab selection and try again.");
            }
            if (message.contains("timed out waiting for enough players")) {
                append_log(client_log_, "Host lobby timed out before enough players arrived.");
            }
        }
        client_session_live_ = false;
        QMetaObject::invokeMethod(
            this,
            [this] {
                if (client_video_controller_) {
                    client_video_controller_->endSession();
                    client_video_controller_.reset();
                }
                client_catalog_status_->setText("Client stopped");
                refresh_game_options_ui();
                // Reap finished worker so the next Connect/Join does not need Stop Client.
                if (client_thread_.joinable() && !client_session_live_.load()) {
                    client_thread_.join();
                }
            },
            Qt::QueuedConnection);
        append_log(client_log_, "Client worker stopped.", GuiLogLevel::Quiet);
    });
}

void MainWindow::stop_client() {
    client_stop_requested_ = true;
    if (client_thread_.joinable()) {
        client_thread_.join();
    }
    client_session_live_ = false;
    // Overlay is fully stopped after join — only then destroy the X11 window.
    if (client_video_controller_) {
        client_video_controller_->endSession();
        client_video_controller_.reset();
    }
    if (disc_control_) {
        std::lock_guard lock(disc_control_->mutex);
        disc_control_->session_active = false;
    }
    if (link_control_) {
        std::lock_guard lock(link_control_->mutex);
        link_control_->session_active = false;
        link_control_->link_capable = false;
    }
    soft_keyboard_.reset();
    soft_keyboard_request_id_ = 0;
    close_pad_on_screen_keyboard();
    heartbeat_prefs_.reset();
    media_resync_.reset();
    if (client_resync_av_ != nullptr) {
        client_resync_av_->setEnabled(false);
    }
}

void MainWindow::stop_client_connect() {
    if (client_connect_thread_.joinable()) {
        client_connect_thread_.join();
    }
}

void MainWindow::send_client_logs_to_host() {
    auto* log = settings_log_ != nullptr ? settings_log_ : client_log_;
    if (client_host_ == nullptr || client_port_ == nullptr) {
        append_log(log, "Send logs: host/port fields missing.", GuiLogLevel::Quiet);
        return;
    }
    const auto host = client_host_->text().trimmed();
    if (host.isEmpty()) {
        append_log(log, "Send logs: select a host first (Client tab).", GuiLogLevel::Quiet);
        return;
    }
    const auto sessions = settings_log_sessions_ != nullptr
        ? static_cast<std::uint32_t>(settings_log_sessions_->value())
        : 3u;
    const auto text = archstreamer::extract_last_log_sessions_from_file(
        gui_log_path(),
        archstreamer::GuiLogSessionMarker,
        sessions);
    if (text.empty()) {
        append_log(log, "Send logs: gui.log is empty or unreadable.", GuiLogLevel::Quiet);
        return;
    }

    archstreamer::ClientLogBundle bundle;
    bundle.username = profile_client_username();
    bundle.session_count = sessions;
    bundle.text.assign(text.begin(), text.end());

    try {
        auto stream = archstreamer::TcpStream::connect_to(
            host.toStdString(),
            static_cast<std::uint16_t>(client_port_->value()));
        stream.send_packet(archstreamer::serialize_packet(bundle));
        const auto reply = stream.receive_packet();
        if (!reply.has_value()) {
            append_log(log, "Send logs: host closed without ack.", GuiLogLevel::Quiet);
            return;
        }
        const auto payload = archstreamer::deserialize_packet(*reply);
        if (const auto* err = std::get_if<archstreamer::ErrorPacket>(&payload); err != nullptr) {
            append_log(log, QString("Send logs: %1").arg(QString::fromStdString(err->message)));
            return;
        }
        append_log(log, "Send logs: unexpected host reply.");
    } catch (const std::exception& error) {
        append_log(
            log,
            QString("Send logs failed: %1").arg(QString::fromUtf8(error.what())),
            GuiLogLevel::Quiet);
    }
}

QString MainWindow::prompt_new_password(const QString& title) {
    bool ok = false;
    const auto first = QInputDialog::getText(
        this,
        title,
        "New password:",
        QLineEdit::Password,
        {},
        &ok);
    if (!ok || first.isEmpty()) {
        return {};
    }
    const auto second = QInputDialog::getText(
        this,
        title,
        "Confirm new password:",
        QLineEdit::Password,
        {},
        &ok);
    if (!ok) {
        return {};
    }
    if (first != second) {
        QMessageBox::warning(this, title, "Passwords do not match.");
        return {};
    }
    return first;
}

void MainWindow::change_profile_password_on_host() {
    auto* log = profile_log_ != nullptr ? profile_log_ : client_log_;
    if (client_host_ == nullptr || client_port_ == nullptr) {
        append_log(log, "Change password: host/port fields missing.", GuiLogLevel::Quiet);
        return;
    }
    const auto host = client_host_->text().trimmed();
    if (host.isEmpty()) {
        append_log(log, "Change password: select a host first (Client tab).", GuiLogLevel::Quiet);
        return;
    }
    QString current;
    if (client_password_ != nullptr && !client_password_->text().isEmpty()) {
        current = client_password_->text();
    } else if (profile_change_current_password_ != nullptr) {
        current = profile_change_current_password_->text();
    }
    if (current.isEmpty()) {
        append_log(
            log,
            "Change password: enter your password on the Client tab, or Current password here.",
            GuiLogLevel::Quiet);
        return;
    }
    const auto new_pw = profile_new_password_ != nullptr ? profile_new_password_->text() : QString{};
    const auto confirm = profile_confirm_password_ != nullptr ? profile_confirm_password_->text() : QString{};
    if (new_pw.isEmpty() || confirm.isEmpty()) {
        append_log(log, "Change password: fill New password and Confirm new.", GuiLogLevel::Quiet);
        return;
    }
    if (new_pw != confirm) {
        append_log(log, "Change password: new passwords do not match.", GuiLogLevel::Quiet);
        return;
    }

    archstreamer::PasswordChange change;
    change.username = profile_client_username();
    change.current_password = current.toStdString();
    change.new_password = new_pw.toStdString();

    try {
        auto stream = archstreamer::TcpStream::connect_to(
            host.toStdString(),
            static_cast<std::uint16_t>(client_port_->value()));
        stream.send_packet(archstreamer::serialize_packet(change));
        const auto reply = stream.receive_packet();
        if (!reply.has_value()) {
            append_log(log, "Change password: host closed without ack.", GuiLogLevel::Quiet);
            return;
        }
        const auto payload = archstreamer::deserialize_packet(*reply);
        if (const auto* err = std::get_if<archstreamer::ErrorPacket>(&payload); err != nullptr) {
            append_log(log, QString("Change password: %1").arg(QString::fromStdString(err->message)));
            if (err->message == "password updated") {
                if (client_password_ != nullptr) {
                    client_password_->setText(new_pw);
                }
                if (profile_change_current_password_ != nullptr) {
                    profile_change_current_password_->clear();
                }
                if (profile_new_password_ != nullptr) {
                    profile_new_password_->clear();
                }
                if (profile_confirm_password_ != nullptr) {
                    profile_confirm_password_->clear();
                }
            }
            return;
        }
        append_log(log, "Change password: unexpected host reply.");
    } catch (const std::exception& error) {
        append_log(
            log,
            QString("Change password failed: %1").arg(QString::fromUtf8(error.what())),
            GuiLogLevel::Quiet);
    }
}

} // namespace archstreamer::gui
