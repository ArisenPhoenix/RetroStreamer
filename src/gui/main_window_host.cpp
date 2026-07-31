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
#ifdef ARCHSTREAMER_HAS_HOST
#include "gui_host_runner.hpp"
#include "host/game_catalog_scanner.hpp"
#include "host/gpu_select.hpp"
#include "host/host_app_config.hpp"
#include "host/media_capture.hpp"
#include "host/standalone_emulator.hpp"
#endif


namespace archstreamer::gui {

#ifdef ARCHSTREAMER_HAS_HOST

QWidget* MainWindow::build_host_tab() {
    auto* page = new QWidget(this);
    auto* root = new QHBoxLayout(page);

    auto* form_box = new QGroupBox("Host Runner", page);
    auto* form = new QFormLayout(form_box);

    host_rom_root_ = new QLineEdit(archstreamer::DefaultRomRoot, form_box);
    host_meta_root_ = new QLineEdit(archstreamer::DefaultMetaRoot, form_box);
    host_control_port_ = new QSpinBox(form_box);
    host_control_port_->setRange(1, 65535);
    host_control_port_->setValue(45555);
    host_input_port_ = new QSpinBox(form_box);
    host_input_port_->setRange(1, 65535);
    host_input_port_->setValue(DefaultInputPort);
    host_video_port_ = new QSpinBox(form_box);
    host_video_port_->setRange(1, 65535);
    host_video_port_->setValue(DefaultVideoPort);
    host_audio_port_ = new QSpinBox(form_box);
    host_audio_port_->setRange(1, 65535);
    host_audio_port_->setValue(DefaultAudioPort);
    host_clients_ = new QSpinBox(form_box);
    host_clients_->setRange(2, 4);
    host_clients_->setValue(2);
    host_clients_->setToolTip(
        "Max concurrent singleplayer sessions (each client gets its own emulator/stream). "
        "Also the Multiplayer lobby size when using Multiplayer mode.");
    host_role_ = new QComboBox(form_box);
    // Viewer first: LAN remote play is the default host path (not seated).
    host_role_->addItem("Viewer", QStringLiteral("viewer"));
    host_role_->addItem("Player", QStringLiteral("player"));
    host_role_->setCurrentIndex(0);
    host_mode_ = new QComboBox(form_box);
    host_mode_->addItems({"Singleplayer", "Multiplayer"});
    host_bridge_controller_ = new QComboBox(form_box);
    host_video_ = new QCheckBox("Stream video", form_box);
    host_video_->setChecked(true);
    host_audio_ = new QCheckBox("Stream audio", form_box);
    host_audio_->setChecked(true);
    host_local_media_ = new QCheckBox("Watch stream locally", form_box);
    host_local_media_->setChecked(false);
    host_local_media_->setToolTip(
        "Receive the host loopback RTP stream (same feed remotes get). "
        "Can be toggled while the host is running. "
        "With Stream audio on, game audio is routed to a silent null sink — "
        "enable this to hear it on the host.");
    host_audio_->setToolTip(
        "Capture RetroArch audio for remotes. Uses a dedicated null sink so the "
        "host speakers stay quiet unless Watch stream locally is enabled.");
    host_advertise_ = new QCheckBox("Advertise on LAN", form_box);
    host_advertise_->setChecked(true);

    form->addRow("ROM root", host_rom_root_);
    form->addRow("Meta root", host_meta_root_);
    form->addRow("Control port", host_control_port_);
    form->addRow("Input port", host_input_port_);
    form->addRow("Video port", host_video_port_);
    form->addRow("Audio port", host_audio_port_);
    form->addRow("Max clients", host_clients_);
    form->addRow("Host role", host_role_);
    form->addRow("Mode", host_mode_);
    form->addRow("Bridge controller", host_bridge_controller_);
    form->addRow("", host_video_);
    form->addRow("", host_audio_);
    form->addRow("", host_local_media_);
    form->addRow("", host_advertise_);

    auto* start = new QPushButton("Start Host", page);
    auto* stop = new QPushButton("Stop Host", page);
    auto* load_games = new QPushButton("Load Games", page);
    auto* refresh_host_controllers_button = new QPushButton("Refresh Controllers", page);
    host_status_ = new QLabel("Host stopped", page);
    host_game_picker_ = new archstreamer::gui::GamePickerWidget(page);
    host_game_picker_->setArtRoot(art_root_path());
    connect(host_game_picker_, &archstreamer::gui::GamePickerWidget::selectionChanged, this, [this] {
        if (host_game_picker_->hasSelection()) {
            persisted_host_game_id_ =
                QString::fromStdString(*host_game_picker_->selectedGameId());
        }
        persist_settings_if_idle();
    });
    connect(start, &QPushButton::clicked, this, [this] {
        remember_session_tab(QStringLiteral("host"));
        start_host();
    });
    connect(stop, &QPushButton::clicked, this, [this] {
        stop_host();
    });
    connect(load_games, &QPushButton::clicked, this, [this] {
        load_host_games();
    });
    connect(refresh_host_controllers_button, &QPushButton::clicked, this, [this] {
        refresh_host_controllers();
    });
    connect(host_rom_root_, &QLineEdit::editingFinished, this, [this] {
        persist_settings_if_idle();
    });
    connect(host_meta_root_, &QLineEdit::editingFinished, this, [this] {
        persist_settings_if_idle();
    });
    connect(host_video_port_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        persist_settings_if_idle();
    });
    connect(host_audio_port_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        persist_settings_if_idle();
    });
    connect(host_clients_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        persist_settings_if_idle();
    });
    connect(host_role_, &QComboBox::currentIndexChanged, this, [this] {
        sync_host_role_and_bridge();
        persist_settings_if_idle();
    });
    connect(host_mode_, &QComboBox::currentIndexChanged, this, [this](int) {
        persist_settings_if_idle();
    });
    connect(host_local_media_, &QCheckBox::toggled, this, [this](bool) {
        sync_host_local_media();
        persist_settings_if_idle();
    });
    connect(host_video_, &QCheckBox::toggled, this, [this](bool) {
        if (!host_video_->isChecked() && !host_audio_->isChecked()) {
            stop_host_local_media();
        } else {
            sync_host_local_media();
        }
        persist_settings_if_idle();
    });
    connect(host_audio_, &QCheckBox::toggled, this, [this](bool) {
        if (!host_video_->isChecked() && !host_audio_->isChecked()) {
            stop_host_local_media();
        } else {
            sync_host_local_media();
        }
        persist_settings_if_idle();
    });
    connect(host_bridge_controller_, &QComboBox::currentIndexChanged, this, [this] {
        if (syncing_host_role_) {
            return;
        }
        // Do not silently flip Viewer → Player; local bridge is opt-in via Host role.
        if (host_role_is_viewer(host_role_) && host_bridge_controller_->currentData().toInt() >= 0) {
            syncing_host_role_ = true;
            host_bridge_controller_->setCurrentIndex(0);
            syncing_host_role_ = false;
            append_log(
                host_log_,
                "Bridge controller needs Host role Player. Staying on Viewer (bridge cleared).");
        }
        persist_settings_if_idle();
    });
    connect(host_control_port_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        client_port_->setValue(value);
        persist_settings_if_idle();
    });
    connect(host_input_port_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        client_input_port_->setValue(value);
        persist_settings_if_idle();
    });
    connect(host_advertise_, &QCheckBox::toggled, this, [this](bool checked) {
        // Only broadcast while a host session is actually running; otherwise this
        // machine shows up in other clients' host lists as a fake peer.
        if (checked && host_process_ != nullptr && host_process_->state() != QProcess::NotRunning) {
            sync_host_advertise(true);
        } else {
            sync_host_advertise(false);
            if (checked) {
                append_log(host_log_, "Advertise armed — broadcasting starts when Host is running.");
            }
        }
        persist_settings_if_idle();
    });

    host_advertise_timer_ = new QTimer(page);
    host_advertise_timer_->setInterval(2000);
    connect(host_advertise_timer_, &QTimer::timeout, this, [this] {
        advertise_host();
    });
    auto* left = new QVBoxLayout();
    left->addWidget(form_box);
    left->addWidget(host_status_);
    left->addWidget(new QLabel("Game", page));
    left->addWidget(host_game_picker_);
    left->addWidget(load_games);
    left->addWidget(refresh_host_controllers_button);
    auto* actions = new QHBoxLayout();
    actions->addWidget(start);
    actions->addWidget(stop);
    left->addLayout(actions);
    left->addStretch();

    host_log_ = new QPlainTextEdit(page);
    host_log_->setObjectName("hostLog");
    host_log_->setReadOnly(true);
    root->addLayout(left, 1);
    root->addWidget(host_log_, 2);
    refresh_host_controllers();
    // load_host_games() runs after load_persisted_settings() so lastGameId applies.
    return page;
}

void MainWindow::refresh_host_controllers() {
    const auto previous = host_bridge_controller_->currentData().toInt();
    host_bridge_controller_->clear();
    host_bridge_controller_->addItem("None", -1);
    try {
        const auto devices = client_app_.list_controllers();
        for (std::size_t index = 0; index < devices.size(); ++index) {
            host_bridge_controller_->addItem(
                QString::fromStdString(std::to_string(index) + ": " + devices[index].name),
                static_cast<int>(index));
        }
        auto restored = false;
        for (int row = 0; row < host_bridge_controller_->count(); ++row) {
            if (host_bridge_controller_->itemData(row).toInt() == previous) {
                host_bridge_controller_->setCurrentIndex(row);
                restored = true;
                break;
            }
        }
        if (!restored) {
            host_bridge_controller_->setCurrentIndex(0);
        }
        append_log(host_log_, QString("Detected %1 host controller(s).").arg(devices.size()));
        if (host_bridge_controller_->currentData().toInt() < 0) {
            append_log(host_log_, "Bridge controller left at None (choose one for Host Player).");
        }
        sync_host_role_and_bridge();
    } catch (const std::exception& error) {
        append_log(host_log_, QString("Host controller scan failed: %1").arg(error.what()), GuiLogLevel::Quiet);
    }
}

void MainWindow::sync_host_role_and_bridge() {
    if (syncing_host_role_ || host_role_ == nullptr || host_bridge_controller_ == nullptr) {
        return;
    }
    syncing_host_role_ = true;
    if (host_role_is_viewer(host_role_) && host_bridge_controller_->currentData().toInt() >= 0) {
        host_bridge_controller_->setCurrentIndex(0);
        append_log(host_log_, "Host Viewer selected; bridge controller cleared to None.");
    }
    // Viewer = dedicated stream host (capture on). Player = local play on the real display.
    if (host_video_ != nullptr && host_audio_ != nullptr && host_local_media_ != nullptr) {
        if (host_role_is_viewer(host_role_)) {
            host_video_->setChecked(true);
            host_audio_->setChecked(true);
        } else {
            host_video_->setChecked(false);
            host_audio_->setChecked(false);
            host_local_media_->setChecked(false);
            append_log(
                host_log_,
                "Host Player: Stream video/audio off so RetroArch stays on this screen. "
                "Use Host Viewer to stream to a same-machine or remote client.");
        }
    }
    syncing_host_role_ = false;
}

void MainWindow::sync_host_advertise(bool enabled) {
    if (enabled) {
        if (!host_announcer_) {
            try {
                host_announcer_ = std::make_unique<archstreamer::HostDiscoveryAnnouncer>(
                    archstreamer::HostAnnouncement{
                        profile_host_name(),
                        static_cast<std::uint16_t>(host_control_port_->value()),
                        static_cast<std::uint16_t>(host_input_port_->value()),
                    });
            } catch (const std::exception& error) {
                append_log(host_log_, QString("Advertise failed: %1").arg(error.what()), GuiLogLevel::Quiet);
                host_advertise_->setChecked(false);
                return;
            }
        }
        host_advertise_timer_->start();
        advertise_host();
        append_log(host_log_, "LAN advertise on UDP 45550 (broadcast + unicast probe replies).");
    } else {
        host_advertise_timer_->stop();
        host_announcer_.reset();
        append_log(host_log_, "LAN advertise stopped.");
    }
}

void MainWindow::advertise_host() {
    if (!host_announcer_) {
        return;
    }
    try {
        host_announcer_->set_announcement(archstreamer::HostAnnouncement{
            profile_host_name(),
            static_cast<std::uint16_t>(host_control_port_->value()),
            static_cast<std::uint16_t>(host_input_port_->value()),
        });
        host_announcer_->advertise();
    } catch (const std::exception& error) {
        append_log(host_log_, QString("Advertise error: %1 (check firewall UDP 45550)").arg(error.what()), GuiLogLevel::Quiet);
    }
}

void MainWindow::load_host_games() {
    try {
        const auto rom_root = std::filesystem::path{host_rom_root_->text().toStdString()};
        const auto meta_root = std::filesystem::path{host_meta_root_->text().toStdString()};
        if (!std::filesystem::exists(rom_root)) {
            host_game_picker_->setCatalog({});
            host_status_->setText("Host stopped; ROM root missing");
            append_log(
                host_log_,
                QString("Load games failed: ROM root does not exist or is not visible "
                        "to this app: %1"
                        " (Flatpak may need: flatpak override --user "
                        "--filesystem=<parent>:ro io.github.ArisenPhoenix.ArchStreamer)")
                    .arg(QString::fromStdString(rom_root.string())),
                GuiLogLevel::Quiet);
            return;
        }
        const auto catalog = archstreamer::scan_game_catalog(
            rom_root,
            archstreamer::LibretroCoreRegistry::ubuntu_defaults(),
            meta_root);
        const auto list = catalog.list();
        host_game_picker_->setCatalog(list);
        if (!list.games.empty()) {
            if (!persisted_host_game_id_.isEmpty()) {
                host_game_picker_->setSelectedGameId(persisted_host_game_id_.toStdString());
            }
            if (!host_game_picker_->hasSelection()) {
                host_game_picker_->setSelectedGameId(list.games.front().id);
                persisted_host_game_id_ =
                    QString::fromStdString(list.games.front().id);
            }
        }
        host_status_->setText(QString("Host stopped; %1 game(s) loaded").arg(list.games.size()));
        append_log(host_log_, QString("Loaded %1 host game(s).").arg(list.games.size()));
        if (list.games.empty()) {
            append_log(
                host_log_,
                QString("No playable titles under %1 (need matching libretro cores "
                        "visible to this app, or Ryujinx/Yuzu for Switch).")
                    .arg(QString::fromStdString(rom_root.string())),
                GuiLogLevel::Quiet);
        }
        if (archstreamer::ryujinx_runtime_available()) {
            if (const auto ryu = archstreamer::resolve_ryujinx(); ryu.has_value()) {
                append_log(
                    host_log_,
                    QString("Switch runtime: Ryujinx (%1) — preferred for Link/LDN")
                        .arg(QString::fromStdString(ryu->path.string())));
            }
        } else if (archstreamer::yuzu_runtime_available()) {
            append_log(
                host_log_,
                QString("Switch runtime: Yuzu (fallback). Install Ryujinx for Local Wireless Link."),
                GuiLogLevel::Quiet);
            append_log(
                host_log_,
                QString::fromStdString(archstreamer::ryujinx_unavailable_message()),
                GuiLogLevel::Quiet);
        } else {
            append_log(
                host_log_,
                QString::fromStdString(archstreamer::switch_runtime_unavailable_message()),
                GuiLogLevel::Quiet);
        }
    } catch (const std::exception& error) {
        host_game_picker_->setCatalog({});
        host_status_->setText("Host stopped; game load failed");
        append_log(host_log_, QString("Load games failed: %1").arg(error.what()), GuiLogLevel::Quiet);
    }
}

void MainWindow::start_host() {
    if (host_process_ != nullptr && host_process_->state() != QProcess::NotRunning) {
        append_log(host_log_, "Host is already running.");
        return;
    }

    if (host_process_ == nullptr) {
        host_process_ = new QProcess(this);
        connect(host_process_, &QProcess::readyReadStandardOutput, this, [this] {
            const auto text = QString::fromLocal8Bit(host_process_->readAllStandardOutput()).trimmed();
            if (!text.isEmpty()) {
                for (const auto& line : text.split('\n')) {
                    append_host_process_log(host_log_, line);
                }
            }
        });
        connect(host_process_, &QProcess::readyReadStandardError, this, [this] {
            const auto text = QString::fromLocal8Bit(host_process_->readAllStandardError()).trimmed();
            if (!text.isEmpty()) {
                for (const auto& line : text.split('\n')) {
                    append_host_process_log(host_log_, line);
                }
            }
        });
        connect(host_process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
            // Drain buffered output that often arrives only as the process dies
            // (flatpak-spawn / distrobox wrappers).
            const auto flush_text = [this](const QByteArray& bytes) {
                const auto text = QString::fromLocal8Bit(bytes).trimmed();
                if (text.isEmpty()) {
                    return;
                }
                for (const auto& line : text.split('\n')) {
                    append_host_process_log(host_log_, line);
                }
            };
            flush_text(host_process_->readAllStandardOutput());
            flush_text(host_process_->readAllStandardError());
            stop_host_local_media();
            sync_host_advertise(false);
            host_status_->setText("Host stopped");
            append_log(host_log_, QString("Host exited: code=%1 status=%2")
                .arg(code)
                .arg(status == QProcess::NormalExit ? "normal" : "crashed"), GuiLogLevel::Quiet);
        });
    }

    const auto bridge_index = host_bridge_controller_->currentData().toInt();
    if (!host_role_is_viewer(host_role_) && bridge_index < 0) {
        host_status_->setText("Host not started");
        append_log(host_log_, "Host role is Player, but no bridge controller is selected.");
        append_log(host_log_, "Select a controller or change Host role to Viewer.");
        return;
    }
    if (host_role_is_viewer(host_role_) && bridge_index >= 0) {
        host_status_->setText("Host not started");
        append_log(host_log_, "Host role is Viewer, but a bridge controller is selected.");
        append_log(host_log_, "Select None for bridge controller or change Host role to Player.");
        return;
    }
    if (!host_role_is_viewer(host_role_) && !host_game_picker_->hasSelection()) {
        append_log(host_log_, "Host Player needs a game selected (used when auto-starting a local seat).");
        return;
    }
    // Viewer hosts are a persistent lobby: clients pick the game. No host game required.

    QStringList args;
    {
        archstreamer::HostAppConfig host_cfg;
        host_cfg.rom_root = host_rom_root_->text().toStdString();
        host_cfg.meta_root = host_meta_root_->text().toStdString();
        host_cfg.art_root = art_root_path();
        host_cfg.control_port = static_cast<std::uint16_t>(host_control_port_->value());
        host_cfg.input_port = static_cast<std::uint16_t>(host_input_port_->value());
        host_cfg.clients = static_cast<std::uint8_t>(host_clients_->value());
        host_cfg.session_timeout_seconds = static_cast<std::uint16_t>(session_timeout_seconds());
        host_cfg.host_role = host_role_is_viewer(host_role_)
            ? archstreamer::ParticipantRole::Viewer
            : archstreamer::ParticipantRole::Player;
        host_cfg.session_mode = selected_mode(host_mode_);
        host_cfg.encode_gpu = selected_encode_gpu_id();
        host_cfg.separate_render_gpu =
            settings_separate_render_gpu_ != nullptr &&
            settings_separate_render_gpu_->isChecked();
        host_cfg.render_gpu = selected_render_gpu_id();
        const auto renderer = selected_graphics_api_id();
        if (renderer == QLatin1String("opengl")) {
            host_cfg.graphics_api = archstreamer::GraphicsApiPreference::OpenGL;
        } else if (renderer == QLatin1String("vulkan")) {
            host_cfg.graphics_api = archstreamer::GraphicsApiPreference::Vulkan;
        } else {
            host_cfg.graphics_api = archstreamer::GraphicsApiPreference::Auto;
        }
        host_cfg.yuzu_resolution_scale = selected_yuzu_resolution_scale();
        host_cfg.retroarch_resolution_scale = selected_retroarch_resolution_scale();
        host_cfg.verbose = current_log_level() == GuiLogLevel::Verbose;
        host_cfg.video = host_video_->isChecked();
        host_cfg.video_port = static_cast<std::uint16_t>(host_video_port_->value());
        host_cfg.audio = host_audio_->isChecked();
        host_cfg.audio_port = static_cast<std::uint16_t>(host_audio_port_->value());
        if (bridge_index >= 0) {
            host_cfg.bridge_controller_index = static_cast<std::size_t>(bridge_index);
        }
        if (host_game_picker_->hasSelection()) {
            host_cfg.selector = *host_game_picker_->selectedGameId();
            const auto selected_id = *host_cfg.selector;
            persisted_host_game_id_ = QString::fromStdString(selected_id);
            // Keep Client Join in sync for local host+client testing.
            persisted_client_game_id_ = persisted_host_game_id_;
            if (client_game_picker_ != nullptr) {
                client_game_picker_->setSelectedGameId(selected_id);
            }
        }
        persist_settings_if_idle();

        for (const auto& arg : archstreamer::host_app_config_to_argv(host_cfg)) {
            args << QString::fromStdString(arg);
        }
    }
    if (host_audio_->isChecked()) {
        append_log(
            host_log_,
            QString("Audio streaming enabled on base UDP port %1 (captures default output monitor).")
                .arg(host_audio_port_->value()));
    }
    args << host_debug_args_;

    append_log(
        host_log_,
        "Persistent lobby: sessions can start/stop without shutting down the host. "
        "Press Stop Host only when you want host_runner to exit.");

    client_port_->setValue(host_control_port_->value());
    client_input_port_->setValue(host_input_port_->value());
    client_mode_->setCurrentIndex(host_mode_->currentIndex());

    QString program;
    QStringList launch_args = args;
    if (running_inside_flatpak()) {
        const auto native = resolve_native_host_runner(
            settings_native_host_runner_ != nullptr ? settings_native_host_runner_->text()
                                                    : QString{});
        if (native.isEmpty()) {
            host_status_->setText("Host failed to start");
            append_log(
                host_log_,
                "Flatpak Host needs a native host_runner. Set Settings → Native host_runner "
                "or ARCHSTREAMER_HOST_RUNNER to a host OS build (gamescope/uinput).",
                GuiLogLevel::Quiet);
            return;
        }
        program = QStringLiteral("flatpak-spawn");
        launch_args.prepend(native);
        launch_args.prepend(QStringLiteral("--host"));
        append_log(
            host_log_,
            QString("Flatpak: spawning native host via flatpak-spawn --host %1").arg(native));
    } else {
        program = host_runner_program();
    }
    host_status_->setText("Host starting");
    host_log_->appendPlainText("Starting " + program + " " + launch_args.join(' '));
    if (bridge_index >= 0) {
        append_log(
            host_log_,
            QString("Host Player as P1; launching when requirements are met (mode=%1).")
                .arg(mode_name(selected_mode(host_mode_))));
    } else {
        append_log(
            host_log_,
            QString("Host Viewer: waiting up to %1s for remote players (mode=%2, max clients=%3).")
                .arg(session_timeout_seconds())
                .arg(mode_name(selected_mode(host_mode_)))
                .arg(host_clients_->value()));
    }
    host_process_->start(program, launch_args);
    if (!host_process_->waitForStarted(3000)) {
        host_status_->setText("Host failed to start");
        host_log_->appendPlainText("Failed to start host_runner: " + host_process_->errorString());
        return;
    }
    host_status_->setText(QString("Host running on port %1").arg(host_control_port_->value()));
    if (host_advertise_ != nullptr && host_advertise_->isChecked()) {
        sync_host_advertise(true);
    }
    // Give host_runner time to bring up capture/fanout before opening the local receiver.
    if (host_local_media_ != nullptr && host_local_media_->isChecked()) {
        QTimer::singleShot(2500, this, [this] {
            sync_host_local_media();
        });
    }
}

void MainWindow::stop_host() {
    stop_host_local_media();
    sync_host_advertise(false);
    if (host_process_ == nullptr || host_process_->state() == QProcess::NotRunning) {
        return;
    }
    host_process_->terminate();
    if (!host_process_->waitForFinished(3000)) {
        host_process_->kill();
        host_process_->waitForFinished(3000);
    }
}

void MainWindow::stop_host_local_media() {
    if (host_local_media_poll_timer_ != nullptr) {
        host_local_media_poll_timer_->stop();
    }
    if (host_local_receiver_) {
        try {
            host_local_receiver_->disconnect();
        } catch (const std::exception& error) {
            append_log(host_log_, QString("Local media stop: %1").arg(error.what()));
        }
        host_local_receiver_.reset();
    }
}

void MainWindow::sync_host_local_media() {
    if (host_local_media_ == nullptr) {
        return;
    }
    const bool want = host_local_media_->isChecked();
    const bool host_up =
        host_process_ != nullptr && host_process_->state() != QProcess::NotRunning;
    const bool streaming = host_video_->isChecked() || host_audio_->isChecked();

    if (!want || !host_up || !streaming) {
        if (host_local_receiver_) {
            append_log(host_log_, "Local stream watch stopped.");
        }
        stop_host_local_media();
        return;
    }

    archstreamer::MediaEndpoint endpoint;
    if (host_video_->isChecked()) {
        endpoint.video_uri = archstreamer::rtp_h264_uri(
            "127.0.0.1",
            static_cast<std::uint16_t>(host_video_port_->value()));
    }
    if (host_audio_->isChecked()) {
        endpoint.audio_uri = archstreamer::rtp_opus_uri(
            "127.0.0.1",
            static_cast<std::uint16_t>(host_audio_port_->value()));
    }

    try {
        stop_host_local_media();
        const bool use_synced =
            client_synced_av_ != nullptr && client_synced_av_->isChecked();
        auto playback = std::make_unique<archstreamer::ClientMediaPlayback>();
        playback->connect(
            endpoint,
            use_synced
                ? archstreamer::ClientMediaPlayback::Strategy::Synced
                : archstreamer::ClientMediaPlayback::Strategy::Legacy);
        host_local_receiver_ = std::move(playback);
        if (host_local_media_poll_timer_ == nullptr) {
            host_local_media_poll_timer_ = new QTimer(this);
            host_local_media_poll_timer_->setInterval(500);
            connect(host_local_media_poll_timer_, &QTimer::timeout, this, [this] {
                if (!host_local_receiver_) {
                    return;
                }
                try {
                    if (host_local_receiver_->poll()) {
                        append_log(host_log_, "Local watch: audio output device changed; playback rebound.");
                    }
                } catch (const std::exception& error) {
                    append_log(
                        host_log_,
                        QString("Local watch audio rebind failed: %1").arg(error.what()),
                        GuiLogLevel::Quiet);
                }
            });
        }
        host_local_media_poll_timer_->start();
        append_log(
            host_log_,
            QString("Watching local stream (video port %1, audio port %2, %3).")
                .arg(host_video_port_->value())
                .arg(host_audio_port_->value())
                .arg(use_synced ? "synced A/V" : "legacy dual pipeline"));
    } catch (const std::exception& error) {
        stop_host_local_media();
        append_log(host_log_, QString("Local stream watch failed: %1").arg(error.what()), GuiLogLevel::Quiet);
    }
}

#endif // ARCHSTREAMER_HAS_HOST

} // namespace archstreamer::gui
