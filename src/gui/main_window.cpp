#include "main_window.hpp"

#include "gui_logging.hpp"
#include "gui_util.hpp"
#include "game_picker_widget.hpp"
#include "host_search_dialog.hpp"
#include "pad_on_screen_keyboard.hpp"
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
#include "client/video_window_geometry.hpp"
#include "common/controller_button_map.hpp"
#include "common/controls_db_pack.hpp"
#include "common/client_debug_log.hpp"
#include "client/controls_sync.hpp"
#include "archstreamer/runtime_cadence/cadence.hpp"
#ifdef ARCHSTREAMER_HAS_HOST
#include "host/user_controls_db.hpp"
#endif

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPixmapCache>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QFileInfo>
#include <QDir>
#include <QCoreApplication>

#include <algorithm>
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
#include "host/save_profile.hpp"
#include "host/standalone_emulator.hpp"
#endif


namespace archstreamer::gui {

MainWindow::MainWindow() {
    // Block persistence until widgets exist and load_persisted_settings finishes.
    // Host/client tab construction refreshes combos and would otherwise save defaults
    // (e.g. lobby wait = 30) before the Settings controls are created.
    restoring_settings_ = true;
    setWindowTitle("ArchStreamer");
    resize(1100, 720);

    tabs_ = new QTabWidget(this);
    tabs_->addTab(build_client_tab(), "Client");
    tabs_->addTab(build_remote_tab(), "Remote");
#ifdef ARCHSTREAMER_HAS_HOST
    tabs_->addTab(build_host_tab(), "Host");
    tabs_->addTab(build_saves_tab(), "Users");
    tabs_->addTab(build_catalog_tab(), "Catalog");
#endif
    tabs_->addTab(build_stream_tab(), "Stream");
    tabs_->addTab(build_controls_tab(), "Controls");
    tabs_->addTab(build_game_options_tab(), "Game Options");
    tabs_->addTab(build_profile_tab(), "Profile");
    tabs_->addTab(build_logs_tab(), "Logs");
    tabs_->addTab(build_settings_tab(), "Settings");
    setCentralWidget(tabs_);
    load_persisted_settings();
#ifdef ARCHSTREAMER_HAS_HOST
    // Host tab construction happens before settings load; apply last game now.
    load_host_games();
#endif
    restore_last_session_tab();
    start_client_host_auto_pick();
    refresh_game_options_ui();
}

MainWindow::~MainWindow() {
    save_persisted_settings();
    stop_client_host_auto_pick();
    // Request stop first so workers unwind before we tear down host media.
    client_stop_requested_ = true;
    stop_client();
    stop_client_connect();
#ifdef ARCHSTREAMER_HAS_HOST
    stop_host();
#endif
    if (art_refresh_thread_.joinable()) {
        art_refresh_thread_.join();
    }
}

void MainWindow::apply_debug_profile(const QString& profile) {
#ifdef ARCHSTREAMER_HAS_HOST
    if (profile != "local-viewer") {
        append_log(host_log_, "Unknown debug profile: " + profile);
        return;
    }

    host_video_->setChecked(true);
    host_audio_->setChecked(true);
    host_role_->setCurrentIndex(0); // Viewer
    host_bridge_controller_->setCurrentIndex(0);
    host_control_port_->setValue(45755);
    host_input_port_->setValue(45456);
    host_video_port_->setValue(5014);
    host_audio_port_->setValue(6014);
    client_role_->setCurrentIndex(1);
    client_port_->setValue(45755);
    client_input_port_->setValue(45456);
    client_players_->setValue(0);
    client_video_->setChecked(true);
    client_audio_->setChecked(true);
    host_debug_args_ = QStringList{
        "--input-port",
        "45456",
        "--virtual-display",
        ":100",
    };
    append_log(host_log_, "Debug profile local-viewer: starting host, fetching catalog, then joining as local viewer.");

    QTimer::singleShot(250, this, [this] {
        start_host();
    });
    QTimer::singleShot(4000, this, [this] {
        connect_client();
    });
    QTimer::singleShot(6500, this, [this] {
        start_client();
    });
#else
    append_log(client_log_, "Debug profiles require a host-capable build (ARCHSTREAMER_BUILD_HOST=ON).");
    Q_UNUSED(profile);
#endif
}

QWidget* MainWindow::build_client_tab() {
    auto* page = new QWidget(this);
    auto* root = new QHBoxLayout(page);

    auto* form_box = new QGroupBox("Client Session", page);
    auto* form = new QFormLayout(form_box);

    client_host_summary_ = new QLabel("No host selected", form_box);
    client_host_summary_->setWordWrap(true);
    client_host_ = new QLineEdit(form_box);
    client_host_->setVisible(false);
    auto* host_row = new QWidget(form_box);
    auto* host_row_layout = new QHBoxLayout(host_row);
    host_row_layout->setContentsMargins(0, 0, 0, 0);
    host_row_layout->addWidget(client_host_summary_, 1);
    auto* select_host = new QPushButton("Select Host…", host_row);
    select_host->setToolTip("Browse LAN hosts. Discovery runs only while this dialog is open.");
    connect(select_host, &QPushButton::clicked, this, [this] {
        open_host_search_dialog();
    });
    host_row_layout->addWidget(select_host);
    auto* this_pc = new QPushButton("This PC", host_row);
    this_pc->setToolTip("Connect to a host running on this machine (127.0.0.1).");
    connect(this_pc, &QPushButton::clicked, this, [this] {
        apply_client_host(QStringLiteral("127.0.0.1"), client_port_->value(), client_input_port_->value(), QStringLiteral("This PC"));
    });
    host_row_layout->addWidget(this_pc);
    client_port_ = new QSpinBox(form_box);
    client_port_->setRange(1, 65535);
    client_port_->setValue(45555);
    client_input_port_ = new QSpinBox(form_box);
    client_input_port_->setRange(1, 65535);
    client_input_port_->setValue(DefaultInputPort);
    client_role_ = new QComboBox(form_box);
    client_role_->addItems({"Player", "Viewer"});
    client_mode_ = new QComboBox(form_box);
    client_mode_->addItems({"Singleplayer", "Multiplayer"});
    client_players_ = new QSpinBox(form_box);
    client_players_->setRange(0, 2);
    client_players_->setValue(1);
    client_send_keyboard_ = new QCheckBox("Send keyboard (Space=FF; P=pause via EmulatorControl)", form_box);
    client_send_keyboard_->setChecked(true);
    client_send_keyboard_->setToolTip(
        "Forwards Space, P, arrows, Enter, Esc, Tab, Backspace, and F1 to the host.\n"
        "Space = fast-forward (hold), F8 = Yuzu continuous FF, P = pause (EmulatorControl), "
        "F1 = RetroArch menu.\n"
        "Works even when the video window has focus (not only this GUI).");
    connect(client_port_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        update_client_host_summary(client_host_label_);
        persist_settings_if_idle();
    });
    connect(client_input_port_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        update_client_host_summary(client_host_label_);
        persist_settings_if_idle();
    });
    connect(client_send_keyboard_, &QCheckBox::toggled, this, [this](bool) {
        persist_settings_if_idle();
    });

    form->addRow("Host", host_row);
    form->addRow("Control port", client_port_);
    form->addRow("Input port", client_input_port_);

    client_password_ = new QLineEdit(form_box);
    client_password_->setEchoMode(QLineEdit::Password);
    client_password_->setPlaceholderText("session only — cleared when GUI exits");
    client_password_->setToolTip(
        "Password for joining sessions. Not saved to disk; re-enter each time you open the app.\n"
        "Existing users often start with archstreamer until changed.");
    form->addRow("Password", client_password_);

    form->addRow("Role", client_role_);
    form->addRow("Mode", client_mode_);
    form->addRow("Players", client_players_);
    form->addRow("", client_send_keyboard_);

    client_game_picker_ = new archstreamer::gui::GamePickerWidget(page);
    client_game_picker_->setArtRoot(art_root_path());
    client_game_picker_->setRecentSettingsKey(QStringLiteral("client/recent_game_ids"));
    connect(client_game_picker_, &archstreamer::gui::GamePickerWidget::selectionChanged, this, [this] {
        if (client_game_picker_->hasSelection()) {
            persisted_client_game_id_ =
                QString::fromStdString(*client_game_picker_->selectedGameId());
        }
        persist_settings_if_idle();
    });
    client_catalog_status_ = new QLabel("Not connected", page);
    client_controllers_ = new QListWidget(page);
    client_controllers_->setSelectionMode(QAbstractItemView::MultiSelection);
    auto* refresh_controllers = new QPushButton("Refresh Controllers", page);
    connect(refresh_controllers, &QPushButton::clicked, this, [this] {
        refresh_client_controllers();
    });

    auto* connect_host = new QPushButton("Connect", page);
    auto* join = new QPushButton("Join Session", page);
    auto* stop = new QPushButton("Stop Client", page);
    connect(client_role_, &QComboBox::currentIndexChanged, this, [this] {
        if (selected_client_role(client_role_) == archstreamer::ClientParticipantRole::Viewer) {
            client_players_->setValue(0);
            client_controllers_->clearSelection();
        } else if (client_players_->value() == 0) {
            client_players_->setValue(1);
        }
        refresh_filtered_client_games();
        persist_settings_if_idle();
    });
    connect(client_mode_, &QComboBox::currentIndexChanged, this, [this] {
        refresh_filtered_client_games();
        persist_settings_if_idle();
    });
    connect(client_players_, qOverload<int>(&QSpinBox::valueChanged), this, [this] {
        refresh_filtered_client_games();
        persist_settings_if_idle();
    });
    connect(connect_host, &QPushButton::clicked, this, [this] {
        remember_session_tab(QStringLiteral("client"));
        connect_client();
    });
    connect(join, &QPushButton::clicked, this, [this] {
        remember_session_tab(QStringLiteral("client"));
        start_client();
    });
    connect(stop, &QPushButton::clicked, this, [this] {
        stop_client();
    });

    auto* left = new QVBoxLayout();
    left->addWidget(form_box);
    left->addWidget(new QLabel("Game", page));
    left->addWidget(client_catalog_status_);
    left->addWidget(client_game_picker_);
    left->addWidget(new QLabel("Controllers", page));
    left->addWidget(client_controllers_, 1);
    left->addWidget(refresh_controllers);
    auto* actions = new QHBoxLayout();
    actions->addWidget(connect_host);
    actions->addWidget(join);
    actions->addWidget(stop);
    left->addLayout(actions);
    left->addStretch();

    client_log_ = new QPlainTextEdit(page);
    client_log_->setObjectName("clientLog");
    client_log_->setReadOnly(true);
    root->addLayout(left, 1);
    root->addWidget(client_log_, 2);

    refresh_client_controllers();
    return page;
}

QWidget* MainWindow::build_stream_tab() {
    auto* page = new QWidget(this);
    auto* root = new QVBoxLayout(page);

    auto* client_box = new QGroupBox("Client stream", page);
    auto* client_form = new QFormLayout(client_box);

    client_stream_quality_ = new QComboBox(client_box);
    client_stream_quality_->addItem("Auto", static_cast<int>(archstreamer::MediaQualityTier::Auto));
    client_stream_quality_->addItem("Low (800 kbps / 20 fps)", static_cast<int>(archstreamer::MediaQualityTier::Low));
    client_stream_quality_->addItem("Medium (3.5 Mbps / 30 fps)", static_cast<int>(archstreamer::MediaQualityTier::Medium));
    client_stream_quality_->addItem("Med-High (8 Mbps / 60 fps)", static_cast<int>(archstreamer::MediaQualityTier::MediumHigh));
    client_stream_quality_->addItem("High (12 Mbps / 60 fps)", static_cast<int>(archstreamer::MediaQualityTier::High));
    client_stream_quality_->addItem("Very-High (25 Mbps / 60 fps)", static_cast<int>(archstreamer::MediaQualityTier::VeryHigh));
    client_stream_quality_->setCurrentIndex(0);
    client_stream_quality_->setToolTip(
        "Preferred bitrate/FPS sent to the host each second.\n"
        "Auto starts at Medium and steps up/down from decode health (~1 Hz heartbeats).\n"
        "Resolution is chosen separately under Stream size.\n"
        "Use Medium or Low on Wi‑Fi / weaker laptops; High/Very-High need a strong link.");
    client_stream_size_ = new QComboBox(client_box);
    client_stream_size_->addItem("Auto (match display height)", static_cast<int>(archstreamer::MediaStreamSize::Auto));
    client_stream_size_->addItem("540p", static_cast<int>(archstreamer::MediaStreamSize::P540));
    client_stream_size_->addItem("720p", static_cast<int>(archstreamer::MediaStreamSize::P720));
    client_stream_size_->addItem("1080p", static_cast<int>(archstreamer::MediaStreamSize::P1080));
    client_stream_size_->addItem("1440p", static_cast<int>(archstreamer::MediaStreamSize::P1440));
    client_stream_size_->setCurrentIndex(0);
    client_stream_size_->setToolTip(
        "Encode height sent to the host (independent of bitrate/FPS).\n"
        "Auto picks from this machine's display height (e.g. 1440p on ultrawide).\n"
        "Host capture resolution should be at least this tall.");
    client_video_ = new QCheckBox("Receive video", client_box);
    client_video_->setChecked(true);
    client_audio_ = new QCheckBox("Receive audio", client_box);
    client_audio_->setChecked(true);
    client_synced_av_ = new QCheckBox("Synced A/V (experimental)", client_box);
    client_synced_av_->setChecked(false);
    client_synced_av_->setToolTip(
        "Optional shared-clock pipeline for lip-sync experiments.\n"
        "Leave off for play (default): dual low-latency receivers.\n"
        "If picture and sound drift, use Resync A/V instead of enabling this.");
    client_resync_av_ = new QPushButton("Resync A/V", client_box);
    client_resync_av_->setToolTip(
        "Restart audio to match the current video (lip-sync recovery).\n"
        "Use if sound drifts ahead after a stutter; video keeps playing.");
    client_resync_av_->setEnabled(false);

    client_form->addRow("Stream quality", client_stream_quality_);
    client_form->addRow("Stream size", client_stream_size_);
    client_form->addRow("", client_video_);
    client_form->addRow("", client_audio_);
    client_form->addRow("", client_synced_av_);
    client_form->addRow("", client_resync_av_);

    connect(client_video_, &QCheckBox::toggled, this, [this](bool) {
        persist_settings_if_idle();
    });
    connect(client_audio_, &QCheckBox::toggled, this, [this](bool) {
        persist_settings_if_idle();
    });
    connect(client_synced_av_, &QCheckBox::toggled, this, [this](bool) {
#ifdef ARCHSTREAMER_HAS_HOST
        if (host_local_media_ != nullptr && host_local_media_->isChecked()) {
            sync_host_local_media();
        }
#endif
        persist_settings_if_idle();
    });
    connect(client_resync_av_, &QPushButton::clicked, this, [this] {
        if (media_resync_) {
            media_resync_->request();
            append_log(client_log_, "Requested A/V resync…");
        } else {
            append_log(client_log_, "Join a session before resyncing A/V.");
        }
    });
    connect(client_stream_quality_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        if (heartbeat_prefs_) {
            heartbeat_prefs_->set_wanted_tier(selected_stream_quality());
        }
        persist_settings_if_idle();
    });
    connect(client_stream_size_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        if (heartbeat_prefs_) {
            heartbeat_prefs_->set_wanted_size(selected_stream_size());
        }
        persist_settings_if_idle();
    });

    root->addWidget(client_box);

#ifdef ARCHSTREAMER_HAS_HOST
    auto* host_box = new QGroupBox("Host encode", page);
    auto* host_form = new QFormLayout(host_box);

    host_capture_resolution_ = new QComboBox(host_box);
    host_capture_resolution_->setEditable(true);
    host_capture_resolution_->addItems({
        QStringLiteral("1280x720"),
        QStringLiteral("1920x1080"),
        QStringLiteral("2560x1440"),
        QStringLiteral("3440x1440"),
        QStringLiteral("3840x2160"),
    });
    host_capture_resolution_->setCurrentText(
        QString::fromStdString(archstreamer::HostAppConfig{}.video_resolution));
    host_capture_resolution_->setToolTip(
        "Virtual display size the game renders into and the host encodes from.\n"
        "Client Stream size is clamped to this height, so an ultrawide client\n"
        "needs a matching capture (e.g. 3440x1440) to fill its screen height.");

    settings_gpu_ = new QComboBox(host_box);
    settings_gpu_->setToolTip(
        "GPU for game render and H.264 encode (normal single-GPU mode).\n"
        "Auto picks the highest-scoring discrete card.\n"
        "Optional: enable Separate render GPU only if you want encode on one card and render on another.");

    settings_separate_render_gpu_ = new QCheckBox("Separate render GPU", host_box);
    settings_separate_render_gpu_->setChecked(false);
    settings_separate_render_gpu_->setToolTip(
        "Advanced: encode stays on Host GPU above; render uses the second dropdown.\n"
        "Leave unchecked for normal single-GPU setups.");

    settings_render_gpu_ = new QComboBox(host_box);
    settings_render_gpu_->setToolTip(
        "Render-only GPU when Separate render GPU is checked.\n"
        "Ignored in normal single-GPU mode.");
    settings_render_gpu_->setEnabled(false);

    settings_renderer_ = new QComboBox(host_box);
    settings_renderer_->addItem("Auto", QStringLiteral("auto"));
    settings_renderer_->addItem("OpenGL", QStringLiteral("opengl"));
    settings_renderer_->addItem("Vulkan", QStringLiteral("vulkan"));
    settings_renderer_->setCurrentIndex(0);
    settings_renderer_->setToolTip(
        "Preferred graphics API for Switch standalone emulators (Ryujinx/Yuzu).\n"
        "Auto: Vulkan on gamescope, OpenGL on VirtualGL.\n"
        "Ignored for RetroArch cores.");

    settings_switch_scale_ = new QComboBox(host_box);
    settings_switch_scale_->addItem("1x native", 1);
    settings_switch_scale_->addItem("2x native", 2);
    settings_switch_scale_->addItem("3x native", 3);
    settings_switch_scale_->addItem("4x native", 4);
    settings_switch_scale_->addItem("5x native", 5);
    settings_switch_scale_->addItem("6x native", 6);
    settings_switch_scale_->setCurrentIndex(0);
    settings_switch_scale_->setToolTip(
        "Switch standalone internal resolution scale (docked native ≈ 1080p).\n"
        "Applied to Ryujinx and Yuzu profiles on Host start.\n"
        "2x/3x supersamples into the stream capture — sharper image, more GPU load.\n"
        "Ignored for RetroArch.");

    settings_retroarch_scale_ = new QComboBox(host_box);
    settings_retroarch_scale_->addItem("1x native", 1);
    settings_retroarch_scale_->addItem("2x native", 2);
    settings_retroarch_scale_->addItem("3x native", 3);
    settings_retroarch_scale_->addItem("4x native", 4);
    settings_retroarch_scale_->addItem("5x native", 5);
    settings_retroarch_scale_->addItem("6x native", 6);
    settings_retroarch_scale_->setCurrentIndex(0);
    settings_retroarch_scale_->setToolTip(
        "RetroArch internal resolution scale for cores that support it\n"
        "(LRPS2, SwanStation, PPSSPP, Dolphin, Citra, Mupen64Plus-Next, Beetle PSX HW).\n"
        "Written to that core's .opt on Host start. Ignored for Switch / other cores.");

    host_form->addRow("Capture resolution", host_capture_resolution_);
    host_form->addRow("Host GPU", settings_gpu_);
    host_form->addRow("", settings_separate_render_gpu_);
    host_form->addRow("Render GPU", settings_render_gpu_);
    host_form->addRow("Standalone renderer", settings_renderer_);
    host_form->addRow("Switch resolution", settings_switch_scale_);
    host_form->addRow("RetroArch resolution", settings_retroarch_scale_);
    refresh_settings_gpus();

    connect(host_capture_resolution_, &QComboBox::currentTextChanged, this, [this](const QString&) {
        persist_settings_if_idle();
    });
    connect(settings_gpu_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        persist_settings_if_idle();
    });
    connect(settings_separate_render_gpu_, &QCheckBox::toggled, this, [this](bool checked) {
        update_separate_render_gpu_visibility();
        (void)checked;
        persist_settings_if_idle();
    });
    connect(settings_render_gpu_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        persist_settings_if_idle();
    });
    connect(settings_renderer_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        persist_settings_if_idle();
    });
    connect(settings_switch_scale_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        persist_settings_if_idle();
    });
    connect(settings_retroarch_scale_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        persist_settings_if_idle();
    });

    root->addWidget(host_box);
#endif

    root->addStretch();
    return page;
}

QWidget* MainWindow::build_controls_tab() {
    auto* page = new QWidget(this);
    auto* root = new QVBoxLayout(page);

    controller_map_prefs_ = std::make_shared<archstreamer::ClientControllerMapPrefs>();
    emulator_control_ = std::make_shared<archstreamer::EmulatorControlBridge>();

    auto* map_box = new QGroupBox("Controller mapping", page);
    auto* map_layout = new QVBoxLayout(map_box);
    auto* map_form = new QFormLayout();
    map_layout->addLayout(map_form);

    game_options_map_family_ = new QComboBox(map_box);
    for (std::size_t i = 0; i < archstreamer::ControllerMapFamilyCount; ++i) {
        const auto family = static_cast<archstreamer::ControllerMapFamily>(i);
        game_options_map_family_->addItem(
            QString::fromUtf8(archstreamer::controller_map_family_title(family).data()),
            QString::fromUtf8(archstreamer::controller_map_family_id(family).data()));
    }
    game_options_map_family_->setToolTip(
        "Per-system family profiles (same families as the mobile overlay).\n"
        "During a session the active profile follows the game's system key.");
    map_form->addRow("System family", game_options_map_family_);

    game_options_swap_nw_ = new QCheckBox("Swap NW (Triangle ↔ Square / Y ↔ X)", map_box);
    game_options_swap_se_ = new QCheckBox("Swap SE (Cross ↔ Circle / A ↔ B)", map_box);
    game_options_swap_nw_->setToolTip(
        "Swap the north and west face buttons before they reach the host.\n"
        "Use this when Triangle/Square (or Y/X) feel reversed for this family.");
    game_options_swap_se_->setToolTip(
        "Swap the south and east face buttons before they reach the host.\n"
        "Use this when Cross/Circle (or A/B) feel reversed for this family.");
    map_form->addRow("", game_options_swap_nw_);
    map_form->addRow("", game_options_swap_se_);

    auto fill_remap = [](QComboBox* combo) {
        const archstreamer::ControllerMapAction actions[] = {
            archstreamer::ControllerMapAction::Default,
            archstreamer::ControllerMapAction::A,
            archstreamer::ControllerMapAction::B,
            archstreamer::ControllerMapAction::X,
            archstreamer::ControllerMapAction::Y,
            archstreamer::ControllerMapAction::L,
            archstreamer::ControllerMapAction::R,
            archstreamer::ControllerMapAction::L2,
            archstreamer::ControllerMapAction::R2,
            archstreamer::ControllerMapAction::Select,
            archstreamer::ControllerMapAction::Start,
            archstreamer::ControllerMapAction::Menu,
            archstreamer::ControllerMapAction::LeftStick,
            archstreamer::ControllerMapAction::RightStick,
            archstreamer::ControllerMapAction::FastForward,
            archstreamer::ControllerMapAction::ScreenSwap,
        };
        for (const auto action : actions) {
            combo->addItem(
                QString::fromUtf8(archstreamer::controller_map_action_title(action).data()),
                QString::fromUtf8(archstreamer::controller_map_action_id(action).data()));
        }
    };

    game_options_remap_select_ = new QComboBox(map_box);
    game_options_remap_start_ = new QComboBox(map_box);
    game_options_remap_l_ = new QComboBox(map_box);
    game_options_remap_r_ = new QComboBox(map_box);
    game_options_remap_l2_ = new QComboBox(map_box);
    game_options_remap_r2_ = new QComboBox(map_box);
    game_options_remap_l3_ = new QComboBox(map_box);
    game_options_remap_r3_ = new QComboBox(map_box);
    fill_remap(game_options_remap_select_);
    fill_remap(game_options_remap_start_);
    fill_remap(game_options_remap_l_);
    fill_remap(game_options_remap_r_);
    fill_remap(game_options_remap_l2_);
    fill_remap(game_options_remap_r2_);
    fill_remap(game_options_remap_l3_);
    fill_remap(game_options_remap_r3_);
    map_form->addRow("Select →", game_options_remap_select_);
    map_form->addRow("Start →", game_options_remap_start_);
    map_form->addRow("L →", game_options_remap_l_);
    map_form->addRow("R →", game_options_remap_r_);
    map_form->addRow("L2 →", game_options_remap_l2_);
    map_form->addRow("R2 →", game_options_remap_r2_);
    map_form->addRow("L3 (stick click) →", game_options_remap_l3_);
    map_form->addRow("R3 (stick click) →", game_options_remap_r3_);

    auto* remap_hint = new QLabel(
        "Remaps apply to physical button presses (e.g. L3 → Fast-forward, Select → Screen swap "
        "for DS). Stick movement, face A/B/X/Y, and the D-pad stay fixed; "
        "use Swap NW/SE for face buttons.",
        map_box);
    remap_hint->setWordWrap(true);
    remap_hint->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    remap_hint->setMinimumHeight(remap_hint->fontMetrics().lineSpacing() * 3 + 12);
    remap_hint->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    map_layout->addWidget(remap_hint);

    connect(game_options_map_family_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        sync_controller_map_editor_ui();
    });
    const auto commit = [this](bool) {
        commit_controller_map_editor_ui();
        persist_settings_if_idle();
    };
    connect(game_options_swap_nw_, &QCheckBox::toggled, this, commit);
    connect(game_options_swap_se_, &QCheckBox::toggled, this, commit);
    const auto connect_remap = [this](QComboBox* combo) {
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
            commit_controller_map_editor_ui();
            persist_settings_if_idle();
        });
    };
    connect_remap(game_options_remap_select_);
    connect_remap(game_options_remap_start_);
    connect_remap(game_options_remap_l_);
    connect_remap(game_options_remap_r_);
    connect_remap(game_options_remap_l2_);
    connect_remap(game_options_remap_r2_);
    connect_remap(game_options_remap_l3_);
    connect_remap(game_options_remap_r3_);

    root->addWidget(map_box);

    auto* sync_box = new QGroupBox("Controls sync", page);
    auto* sync_layout = new QVBoxLayout(sync_box);
    controls_sync_status_ = new QLabel(
        "Pull/Push the controller map for your Profile username to the host "
        "(Client tab host + password). Connect is not required — this opens its own lobby session.",
        sync_box);
    controls_sync_status_->setWordWrap(true);
    sync_layout->addWidget(controls_sync_status_);
    auto* sync_row = new QWidget(sync_box);
    auto* sync_buttons = new QHBoxLayout(sync_row);
    sync_buttons->setContentsMargins(0, 0, 0, 0);
    controls_sync_pull_ = new QPushButton("Pull from host", sync_row);
    controls_sync_push_ = new QPushButton("Push to host", sync_row);
    controls_sync_pull_->setToolTip(
        "Download this username's controls.sqlite from the host save profile and apply locally.");
    controls_sync_push_->setToolTip(
        "Upload the local controller map (and any stored overlay profiles) to the host profile.");
    sync_buttons->addWidget(controls_sync_pull_);
    sync_buttons->addWidget(controls_sync_push_);
    sync_buttons->addStretch(1);
    sync_layout->addWidget(sync_row);
    connect(controls_sync_pull_, &QPushButton::clicked, this, [this] {
        pull_controls_from_host();
    });
    connect(controls_sync_push_, &QPushButton::clicked, this, [this] {
        push_controls_to_host();
    });
    root->addWidget(sync_box);

    game_options_pad_osk_ = new QPushButton(QStringLiteral("Pad on-screen keyboard"), page);
    game_options_pad_osk_->setCheckable(true);
    game_options_pad_osk_->setToolTip(
        QStringLiteral(
            "Opens a controller-friendly letter keyboard over the game video.\n"
            "During a Switch session, Done sends the text to the host so it can fill "
            "any open software-keyboard dialog (escape hatch if auto-prompt misses).\n"
            "Fullscreen video briefly goes windowed so the keyboard stays visible, "
            "then returns to fullscreen when you close it.\n"
            "Cancel closes without sending; click this button again to close."));
    connect(game_options_pad_osk_, &QPushButton::toggled, this, [this](bool checked) {
        toggle_pad_on_screen_keyboard(checked);
    });
    root->addWidget(game_options_pad_osk_);
    root->addStretch();
    sync_controller_map_editor_ui();
    return page;
}

QWidget* MainWindow::build_game_options_tab() {
    auto* page = new QWidget(this);
    auto* root = new QVBoxLayout(page);

    // Emulator control bridge is owned by Controls; create a fallback if Controls
    // was not built (should not happen with current tab order).
    if (!emulator_control_) {
        emulator_control_ = std::make_shared<archstreamer::EmulatorControlBridge>();
    }

    auto* form_box = new QGroupBox("Disc control", page);
    auto* form = new QFormLayout(form_box);
    game_options_status_ = new QLabel(
        "Join a multi-disc session (.m3u) to swap discs.",
        form_box);
    game_options_status_->setWordWrap(true);
    game_options_disc_ = new QComboBox(form_box);
    game_options_disc_->setEnabled(false);
    auto* swap = new QPushButton("Switch to selected disc", form_box);
    swap->setEnabled(false);
    game_options_swap_ = swap;
    auto* prev = new QPushButton("Previous disc", form_box);
    auto* next = new QPushButton("Next disc", form_box);
    prev->setEnabled(false);
    next->setEnabled(false);
    game_options_prev_ = prev;
    game_options_next_ = next;

    form->addRow("Status", game_options_status_);
    form->addRow("Disc", game_options_disc_);
    form->addRow("", swap);
    auto* nav = new QHBoxLayout();
    nav->addWidget(prev);
    nav->addWidget(next);
    form->addRow("", nav);

    connect(swap, &QPushButton::clicked, this, [this] {
        if (!disc_control_ || game_options_disc_ == nullptr) {
            return;
        }
        const auto index = game_options_disc_->currentData().toInt();
        if (index < 0) {
            return;
        }
        disc_control_->request_set_index(static_cast<std::uint8_t>(index));
        append_log(client_log_, QString("Requested disc index %1").arg(index));
    });
    connect(prev, &QPushButton::clicked, this, [this] {
        if (disc_control_) {
            disc_control_->request_prev();
            append_log(client_log_, "Requested previous disc");
        }
    });
    connect(next, &QPushButton::clicked, this, [this] {
        if (disc_control_) {
            disc_control_->request_next();
            append_log(client_log_, "Requested next disc");
        }
    });

    game_options_poll_timer_ = new QTimer(page);
    game_options_poll_timer_->setInterval(500);
    connect(game_options_poll_timer_, &QTimer::timeout, this, [this] {
        refresh_game_options_ui();
        if (soft_keyboard_) {
            if (const auto request = soft_keyboard_->take_request(); request.has_value()) {
                open_soft_keyboard_from_host(*request);
            }
        }
        if (!disc_control_) {
            return;
        }
        if (const auto response = disc_control_->take_response(); response.has_value()) {
            append_log(
                client_log_,
                response->ok
                    ? QString("Disc control: %1").arg(QString::fromStdString(response->message))
                    : QString("Disc control failed: %1").arg(QString::fromStdString(response->message)),
                response->ok ? GuiLogLevel::Normal : GuiLogLevel::Quiet);
            if (response->ok && game_options_disc_ != nullptr) {
                const auto index = game_options_disc_->findData(static_cast<int>(response->disc_index));
                if (index >= 0) {
                    game_options_disc_->setCurrentIndex(index);
                }
            }
        }
    });
    game_options_poll_timer_->start();

    root->addWidget(form_box);

    auto* link_box = new QGroupBox("Link with player", page);
    auto* link_form = new QFormLayout(link_box);
    game_options_link_status_ = new QLabel(
        "Join a link-capable session (GBA / DS / Switch) to request a peer.",
        link_box);
    game_options_link_status_->setWordWrap(true);
    game_options_link_user_ = new QLineEdit(link_box);
    game_options_link_user_->setPlaceholderText("Peer username");
    game_options_link_user_->setEnabled(false);
    auto* link_request = new QPushButton("Request link", link_box);
    link_request->setEnabled(false);
    game_options_link_request_ = link_request;
    auto* link_cancel = new QPushButton("Cancel", link_box);
    link_cancel->setEnabled(false);
    game_options_link_cancel_ = link_cancel;

    link_form->addRow("Status", game_options_link_status_);
    link_form->addRow("Username", game_options_link_user_);
    auto* link_nav = new QHBoxLayout();
    link_nav->addWidget(link_request);
    link_nav->addWidget(link_cancel);
    link_form->addRow("", link_nav);

    connect(link_request, &QPushButton::clicked, this, [this] {
        if (!link_control_ || game_options_link_user_ == nullptr) {
            return;
        }
        const auto target = game_options_link_user_->text().trimmed().toStdString();
        if (target.empty()) {
            append_log(client_log_, "Link username is empty", GuiLogLevel::Quiet);
            return;
        }
        link_control_->request_link(target);
        append_log(
            client_log_,
            QString("Requested link with %1").arg(QString::fromStdString(target)));
    });
    connect(link_cancel, &QPushButton::clicked, this, [this] {
        if (link_control_) {
            link_control_->cancel_link();
            append_log(client_log_, "Cancelled link request");
        }
    });

    connect(game_options_poll_timer_, &QTimer::timeout, this, [this] {
        if (!link_control_) {
            return;
        }
        if (const auto response = link_control_->take_response(); response.has_value()) {
            const QString message = response->ok
                ? QString("Link: %1").arg(QString::fromStdString(response->message))
                : QString("Link failed: %1").arg(QString::fromStdString(response->message));
            append_log(
                client_log_,
                message,
                response->ok ? GuiLogLevel::Normal : GuiLogLevel::Quiet);
            if (game_options_link_status_ != nullptr) {
                game_options_link_status_->setText(message);
            }
        }
    });

    root->addWidget(link_box);
    root->addStretch();
    return page;
}

ControllerMapFamily MainWindow::selected_controller_map_family() const {
    if (game_options_map_family_ == nullptr) {
        return ControllerMapFamily::Standard;
    }
    const auto id = game_options_map_family_->currentData().toString().toStdString();
    return controller_map_family_from_id(id).value_or(ControllerMapFamily::Standard);
}

void MainWindow::sync_controller_map_editor_ui() {
    if (!controller_map_prefs_ || game_options_swap_nw_ == nullptr) {
        return;
    }
    const auto profile = controller_map_prefs_->profile(selected_controller_map_family());

    const QSignalBlocker block_nw(game_options_swap_nw_);
    const QSignalBlocker block_se(game_options_swap_se_);
    game_options_swap_nw_->setChecked(profile.swap_nw);
    game_options_swap_se_->setChecked(profile.swap_se);

    const auto set_remap = [](QComboBox* combo, ControllerMapAction action) {
        if (combo == nullptr) {
            return;
        }
        const QSignalBlocker blocker(combo);
        const auto id = QString::fromUtf8(controller_map_action_id(action).data());
        const auto index = combo->findData(id);
        combo->setCurrentIndex(index >= 0 ? index : 0);
    };
    set_remap(game_options_remap_select_, profile.select);
    set_remap(game_options_remap_start_, profile.start);
    set_remap(game_options_remap_l_, profile.l);
    set_remap(game_options_remap_r_, profile.r);
    set_remap(game_options_remap_l2_, profile.l2);
    set_remap(game_options_remap_r2_, profile.r2);
    set_remap(game_options_remap_l3_, profile.l3);
    set_remap(game_options_remap_r3_, profile.r3);
}

void MainWindow::commit_controller_map_editor_ui() {
    if (!controller_map_prefs_ || game_options_swap_nw_ == nullptr) {
        return;
    }
    const auto read_action = [](QComboBox* combo) {
        if (combo == nullptr) {
            return ControllerMapAction::Default;
        }
        return controller_map_action_from_id(combo->currentData().toString().toStdString())
            .value_or(ControllerMapAction::Default);
    };

    ControllerMapProfile profile;
    profile.swap_nw = game_options_swap_nw_->isChecked();
    profile.swap_se = game_options_swap_se_->isChecked();
    profile.select = read_action(game_options_remap_select_);
    profile.start = read_action(game_options_remap_start_);
    profile.l = read_action(game_options_remap_l_);
    profile.r = read_action(game_options_remap_r_);
    profile.l2 = read_action(game_options_remap_l2_);
    profile.r2 = read_action(game_options_remap_r2_);
    profile.l3 = read_action(game_options_remap_l3_);
    profile.r3 = read_action(game_options_remap_r3_);
    controller_map_prefs_->set_profile(selected_controller_map_family(), profile);
    save_controller_map_document();
}

std::filesystem::path MainWindow::controller_map_file_path() const {
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return (std::filesystem::path(root.toStdString()) / std::string(archstreamer::ControllerMapFileName));
}

namespace {

#ifdef ARCHSTREAMER_HAS_HOST
bool upsert_controller_map_to_profile(
    const std::filesystem::path& save_root,
    const std::string& username,
    const archstreamer::ControllerMapDocument& document) {
    if (save_root.empty() || username.empty()) {
        return false;
    }
    archstreamer::UserControlsRow row;
    row.username = username;
    row.kind = std::string(archstreamer::kControlsKindButtonMap);
    row.document_json = archstreamer::controller_map_document_to_json(document);
    row.version = archstreamer::ControllerMapDocumentVersion;
    return archstreamer::upsert_user_controls_row(
        archstreamer::user_controls_db_path_for(save_root, username),
        row);
}

std::optional<archstreamer::ControllerMapDocument> load_controller_map_from_profile(
    const std::filesystem::path& save_root,
    const std::string& username) {
    if (save_root.empty() || username.empty()) {
        return std::nullopt;
    }
    auto found = archstreamer::find_user_controls_row(
        archstreamer::user_controls_db_path_for(save_root, username),
        username,
        archstreamer::kControlsKindButtonMap);
    if (!found.has_value()) {
        return std::nullopt;
    }
    return archstreamer::controller_map_document_from_json(found->document_json);
}
#endif

bool upsert_controller_map_to_cadence(
    const std::string& username,
    const archstreamer::ControllerMapDocument& document) {
    auto store = archstreamer::cadence::make_runtime_store();
    if (!store || !store->ensure_ready()) {
        return false;
    }
    archstreamer::cadence::ControlsRecord controls;
    controls.username = username;
    controls.kind = std::string(archstreamer::cadence::kControlsKindButtonMap);
    controls.document_json = archstreamer::controller_map_document_to_json(document);
    controls.version = archstreamer::ControllerMapDocumentVersion;
    return store->upsert_controls(controls);
}

std::optional<archstreamer::ControllerMapDocument> load_controller_map_from_cadence(
    const std::string& username) {
    auto store = archstreamer::cadence::make_runtime_store();
    if (!store || !store->ensure_ready()) {
        return std::nullopt;
    }
    const auto kind = std::string(archstreamer::cadence::kControlsKindButtonMap);
    auto found = store->find_controls(username, kind);
    if (!found.has_value() &&
        username != archstreamer::cadence::kControlsDefaultUsername) {
        found = store->find_controls(
            std::string(archstreamer::cadence::kControlsDefaultUsername),
            kind);
    }
    if (!found.has_value()) {
        return std::nullopt;
    }
    return archstreamer::controller_map_document_from_json(found->document_json);
}

} // namespace

void MainWindow::load_controller_map_document() {
    if (!controller_map_prefs_) {
        return;
    }

    const auto apply_document = [this](const archstreamer::ControllerMapDocument& document) {
        for (std::size_t i = 0; i < archstreamer::ControllerMapFamilyCount; ++i) {
            const auto family = static_cast<archstreamer::ControllerMapFamily>(i);
            controller_map_prefs_->set_profile(family, document.profile(family));
        }
        sync_controller_map_editor_ui();
    };

    auto username = profile_client_username();
    if (username.empty()) {
        username = std::string(archstreamer::cadence::kControlsDefaultUsername);
    }
#ifdef ARCHSTREAMER_HAS_HOST
    if (auto document = load_controller_map_from_profile(save_root_path(), username);
        document.has_value()) {
        apply_document(*document);
        return;
    }
#endif
    if (auto document = load_controller_map_from_cadence(username); document.has_value()) {
        apply_document(*document);
#ifdef ARCHSTREAMER_HAS_HOST
        (void)upsert_controller_map_to_profile(save_root_path(), username, *document);
#endif
        return;
    }

    // One-shot import: AppConfig JSON → profile controls.sqlite.
    const auto path = controller_map_file_path();
    if (auto document = archstreamer::controller_map_document_load_file(path); document.has_value()) {
        apply_document(*document);
#ifdef ARCHSTREAMER_HAS_HOST
        if (!upsert_controller_map_to_profile(save_root_path(), username, *document)) {
            (void)upsert_controller_map_to_cadence(username, *document);
        }
#else
        (void)upsert_controller_map_to_cadence(username, *document);
#endif
        return;
    }

    // Migrate legacy QSettings keys into profile store (and leave a JSON backup once).
    QSettings settings("ArchStreamer", "ArchStreamer");
    archstreamer::ControllerMapDocument document;
    const bool legacy_nw = settings.value("client/swapFaceNw", false).toBool();
    const bool legacy_se = settings.value("client/swapFaceSe", false).toBool();
    bool migrated = false;
    for (std::size_t i = 0; i < archstreamer::ControllerMapFamilyCount; ++i) {
        const auto family = static_cast<archstreamer::ControllerMapFamily>(i);
        const auto prefix = QString("client/buttonMap/%1/")
            .arg(QString::fromUtf8(archstreamer::controller_map_family_id(family).data()));
        archstreamer::ControllerMapProfile profile;
        const bool has_family = settings.contains(prefix + "swapNw") ||
            settings.contains(prefix + "select");
        if (has_family || legacy_nw || legacy_se) {
            migrated = true;
        }
        profile.swap_nw = settings.value(
            prefix + "swapNw",
            has_family ? false : legacy_nw).toBool();
        profile.swap_se = settings.value(
            prefix + "swapSe",
            has_family ? false : legacy_se).toBool();
        const auto load_action = [&](const char* key) {
            const auto id = settings.value(prefix + key, "default").toString().toStdString();
            return archstreamer::controller_map_action_from_id(id)
                .value_or(archstreamer::ControllerMapAction::Default);
        };
        profile.select = load_action("select");
        profile.start = load_action("start");
        profile.l = load_action("l");
        profile.r = load_action("r");
        profile.l2 = load_action("l2");
        profile.r2 = load_action("r2");
        profile.l3 = load_action("l3");
        profile.r3 = load_action("r3");
        document.profile(family) = profile;
        controller_map_prefs_->set_profile(family, profile);
    }
    if (migrated) {
#ifdef ARCHSTREAMER_HAS_HOST
        if (!upsert_controller_map_to_profile(save_root_path(), username, document)) {
            (void)upsert_controller_map_to_cadence(username, document);
        }
#else
        (void)upsert_controller_map_to_cadence(username, document);
#endif
        archstreamer::controller_map_document_save_file(path, document);
    }
    sync_controller_map_editor_ui();
}

void MainWindow::save_controller_map_document() {
    if (!controller_map_prefs_) {
        return;
    }
    archstreamer::ControllerMapDocument document;
    for (std::size_t i = 0; i < archstreamer::ControllerMapFamilyCount; ++i) {
        const auto family = static_cast<archstreamer::ControllerMapFamily>(i);
        document.profile(family) = controller_map_prefs_->profile(family);
    }
    auto username = profile_client_username();
    if (username.empty()) {
        username = std::string(archstreamer::cadence::kControlsDefaultUsername);
    }
#ifdef ARCHSTREAMER_HAS_HOST
    if (!upsert_controller_map_to_profile(save_root_path(), username, document)) {
        if (!upsert_controller_map_to_cadence(username, document)) {
            archstreamer::controller_map_document_save_file(controller_map_file_path(), document);
        }
    }
#else
    if (!upsert_controller_map_to_cadence(username, document)) {
        archstreamer::controller_map_document_save_file(controller_map_file_path(), document);
    }
#endif

    // Keep legacy keys in sync for older builds.
    const auto standard = document.profile(archstreamer::ControllerMapFamily::Standard);
    QSettings settings("ArchStreamer", "ArchStreamer");
    settings.setValue("client/swapFaceNw", standard.swap_nw);
    settings.setValue("client/swapFaceSe", standard.swap_se);
}

void MainWindow::set_controls_sync_status(const QString& text) {
    if (controls_sync_status_ != nullptr) {
        controls_sync_status_->setText(text);
    }
    append_log(client_log_, QStringLiteral("[controls] %1").arg(text));
}

void MainWindow::set_controls_sync_busy(bool busy) {
    controls_sync_busy_ = busy;
    if (controls_sync_pull_ != nullptr) {
        controls_sync_pull_->setEnabled(!busy);
    }
    if (controls_sync_push_ != nullptr) {
        controls_sync_push_->setEnabled(!busy);
    }
}

void MainWindow::pull_controls_from_host() {
    if (controls_sync_busy_) {
        return;
    }
    if (client_host_ == nullptr || client_port_ == nullptr) {
        set_controls_sync_status(QStringLiteral("Client tab is not available."));
        return;
    }
    const auto username = profile_client_username();
    if (username.empty()) {
        set_controls_sync_status(QStringLiteral("Set a Profile username before pulling controls."));
        return;
    }
    const auto host = client_host_->text().trimmed().toStdString();
    if (host.empty()) {
        set_controls_sync_status(QStringLiteral("Set the Client host address before pulling."));
        return;
    }
    const auto password = client_password_ != nullptr
        ? client_password_->text().toStdString()
        : std::string{};
    const auto port = static_cast<std::uint16_t>(client_port_->value());

    set_controls_sync_busy(true);
    set_controls_sync_status(QStringLiteral("Pulling controls…"));
    std::thread([this, host, port, username, password] {
        const auto result = archstreamer::pull_controls_db_from_host(host, port, username, password);
        QMetaObject::invokeMethod(this, [this, result, username] {
            if (!result.ok) {
                set_controls_sync_status(
                    QStringLiteral("Pull failed: %1").arg(QString::fromStdString(result.message)));
                set_controls_sync_busy(false);
                return;
            }
            if (!result.found) {
                set_controls_sync_status(QString::fromStdString(result.message));
                set_controls_sync_busy(false);
                return;
            }
            std::string error;
            auto rows = archstreamer::import_controls_db_pack(result.db_bytes, username, &error);
            if (!rows.has_value()) {
                set_controls_sync_status(
                    QStringLiteral("Pull failed: %1")
                        .arg(QString::fromStdString(error.empty() ? "invalid pack" : error)));
                set_controls_sync_busy(false);
                return;
            }

            auto store = archstreamer::cadence::make_runtime_store();
            if (!store || !store->ensure_ready()) {
                set_controls_sync_status(QStringLiteral("Pull failed: local controls store unavailable."));
                set_controls_sync_busy(false);
                return;
            }
            for (const auto& row : *rows) {
                archstreamer::cadence::ControlsRecord controls;
                controls.username = row.username;
                controls.kind = row.kind;
                controls.document_json = row.document_json;
                controls.version = row.version;
                controls.updated_at = row.updated_at;
                (void)store->upsert_controls(controls);
            }

            // Refresh editor from the pulled button_map when present.
            for (const auto& row : *rows) {
                if (row.kind != archstreamer::kControlsDbPackKindButtonMap) {
                    continue;
                }
                if (auto document = archstreamer::controller_map_document_from_json(row.document_json);
                    document.has_value() && controller_map_prefs_) {
                    for (std::size_t i = 0; i < archstreamer::ControllerMapFamilyCount; ++i) {
                        const auto family = static_cast<archstreamer::ControllerMapFamily>(i);
                        controller_map_prefs_->set_profile(family, document->profile(family));
                    }
                    sync_controller_map_editor_ui();
                    save_controller_map_document();
                }
                break;
            }

            set_controls_sync_status(QString::fromStdString(result.message));
            set_controls_sync_busy(false);
        }, Qt::QueuedConnection);
    }).detach();
}

void MainWindow::push_controls_to_host() {
    if (controls_sync_busy_) {
        return;
    }
    if (client_host_ == nullptr || client_port_ == nullptr) {
        set_controls_sync_status(QStringLiteral("Client tab is not available."));
        return;
    }
    const auto username = profile_client_username();
    if (username.empty()) {
        set_controls_sync_status(QStringLiteral("Set a Profile username before pushing controls."));
        return;
    }
    const auto host = client_host_->text().trimmed().toStdString();
    if (host.empty()) {
        set_controls_sync_status(QStringLiteral("Set the Client host address before pushing."));
        return;
    }
    const auto password = client_password_ != nullptr
        ? client_password_->text().toStdString()
        : std::string{};
    const auto port = static_cast<std::uint16_t>(client_port_->value());

    commit_controller_map_editor_ui();
    save_controller_map_document();

    std::vector<archstreamer::ControlsDbPackRow> rows;
    auto store = archstreamer::cadence::make_runtime_store();
    if (store && store->ensure_ready()) {
        for (const auto& kind : {
                 std::string(archstreamer::cadence::kControlsKindButtonMap),
                 std::string(archstreamer::cadence::kControlsKindOverlayProfiles),
             }) {
            if (auto found = store->find_controls(username, kind); found.has_value()) {
                archstreamer::ControlsDbPackRow row;
                row.username = found->username;
                row.kind = found->kind;
                row.document_json = found->document_json;
                row.version = found->version;
                row.updated_at = found->updated_at;
                rows.push_back(std::move(row));
            }
        }
    }
    if (rows.empty() && controller_map_prefs_) {
        archstreamer::ControllerMapDocument document;
        for (std::size_t i = 0; i < archstreamer::ControllerMapFamilyCount; ++i) {
            const auto family = static_cast<archstreamer::ControllerMapFamily>(i);
            document.profile(family) = controller_map_prefs_->profile(family);
        }
        archstreamer::ControlsDbPackRow row;
        row.username = username;
        row.kind = std::string(archstreamer::kControlsDbPackKindButtonMap);
        row.document_json = archstreamer::controller_map_document_to_json(document);
        row.version = archstreamer::ControllerMapDocumentVersion;
        rows.push_back(std::move(row));
    }

    std::string error;
    auto bytes = archstreamer::export_controls_db_pack(username, rows, &error);
    if (bytes.empty()) {
        set_controls_sync_status(
            QStringLiteral("Push failed: %1")
                .arg(QString::fromStdString(error.empty() ? "empty pack" : error)));
        return;
    }

    set_controls_sync_busy(true);
    set_controls_sync_status(QStringLiteral("Pushing controls…"));
    std::thread([this, host, port, username, password, bytes = std::move(bytes)]() mutable {
        const auto result =
            archstreamer::push_controls_db_to_host(host, port, username, password, bytes);
        QMetaObject::invokeMethod(this, [this, result] {
            set_controls_sync_status(
                result.ok
                    ? QString::fromStdString(result.message)
                    : QStringLiteral("Push failed: %1").arg(QString::fromStdString(result.message)));
            set_controls_sync_busy(false);
        }, Qt::QueuedConnection);
    }).detach();
}

QWidget* MainWindow::build_profile_tab() {
    auto* page = new QWidget(this);
    auto* root = new QHBoxLayout(page);

    auto* form_box = new QGroupBox("Identity", page);
    auto* form = new QFormLayout(form_box);

    profile_username_ = new QLineEdit(form_box);
    profile_host_name_ = new QLineEdit(form_box);
    {
        const auto username = QString::fromStdString(archstreamer::current_username());
        const auto initial = username.isEmpty() ? QStringLiteral("local") : username;
        profile_username_->setText(initial);
        profile_host_name_->setText(initial);
    }
    profile_username_->setToolTip("Name shown when you join a session as a client.");
    profile_host_name_->setToolTip("Name advertised on LAN when you run Host (can differ from Username).");

    profile_steam_account_ = new QLineEdit(form_box);
    profile_steam_account_->setPlaceholderText("auto-detect if empty");
    auto* steam_row = new QWidget(form_box);
    auto* steam_layout = new QHBoxLayout(steam_row);
    steam_layout->setContentsMargins(0, 0, 0, 0);
    auto* detect_steam = new QPushButton("Detect", steam_row);
    steam_layout->addWidget(profile_steam_account_, 1);
    steam_layout->addWidget(detect_steam);

    form->addRow("Username", profile_username_);
    form->addRow("Host name", profile_host_name_);
    form->addRow("Steam account ID", steam_row);

    auto* change_box = new QGroupBox("Change password", page);
    auto* change_form = new QFormLayout(change_box);
    profile_change_current_password_ = new QLineEdit(change_box);
    profile_change_current_password_->setEchoMode(QLineEdit::Password);
    profile_change_current_password_->setPlaceholderText("required when Client password is empty");
    profile_change_current_password_->setToolTip(
        "Only needed if you have not entered your password on the Client tab yet.");
    change_form->addRow("Current password", profile_change_current_password_);
    profile_change_current_row_ = profile_change_current_password_;
    const auto sync_change_current_visibility = [this, change_form] {
        const bool show =
            client_password_ == nullptr || client_password_->text().isEmpty();
        if (profile_change_current_password_ != nullptr) {
            profile_change_current_password_->setVisible(show);
            if (QWidget* label = change_form->labelForField(profile_change_current_password_)) {
                label->setVisible(show);
            }
        }
    };
    sync_change_current_visibility();
    if (client_password_ != nullptr) {
        connect(client_password_, &QLineEdit::textChanged, this, [sync_change_current_visibility](const QString&) {
            sync_change_current_visibility();
        });
    }

    profile_new_password_ = new QLineEdit(change_box);
    profile_new_password_->setEchoMode(QLineEdit::Password);
    profile_confirm_password_ = new QLineEdit(change_box);
    profile_confirm_password_->setEchoMode(QLineEdit::Password);
    change_form->addRow("New password", profile_new_password_);
    change_form->addRow("Confirm new", profile_confirm_password_);
    profile_change_password_ = new QPushButton("Change password on host", change_box);
    change_form->addRow("", profile_change_password_);
    connect(profile_change_password_, &QPushButton::clicked, this, [this] {
        change_profile_password_on_host();
    });

    connect(profile_username_, &QLineEdit::editingFinished, this, [this] {
        persist_settings_if_idle();
    });
    connect(profile_host_name_, &QLineEdit::editingFinished, this, [this] {
        persist_settings_if_idle();
#ifdef ARCHSTREAMER_HAS_HOST
        if (host_advertise_ != nullptr && host_advertise_->isChecked() &&
            host_process_ != nullptr && host_process_->state() != QProcess::NotRunning) {
            sync_host_advertise(true);
        }
#endif
    });
    connect(profile_steam_account_, &QLineEdit::editingFinished, this, [this] {
        persist_settings_if_idle();
    });
    connect(detect_steam, &QPushButton::clicked, this, [this] {
        detect_steam_account();
    });

    auto* left = new QVBoxLayout();
    left->addWidget(form_box);
    left->addWidget(change_box);
    left->addWidget(new QLabel(
        "Username is used when joining as a client.\n"
        "Host name is what others see in Select Host (LAN advertise).\n"
        "They default to the same value; set them apart if you host and play under different identities.\n"
        "Enter your session password on the Client tab. Use this page to change it on the host.\n"
        "Steam account ID is used for art import (leave blank to auto-detect).",
        page));
    left->addStretch();

    profile_log_ = new QPlainTextEdit(page);
    profile_log_->setReadOnly(true);
    root->addLayout(left, 1);
    root->addWidget(profile_log_, 2);
    return page;
}

QWidget* MainWindow::build_logs_tab() {
    auto* page = new QWidget(this);
    auto* root = new QHBoxLayout(page);

    auto* form_box = new QGroupBox("Log handling", page);
    auto* form = new QFormLayout(form_box);

    settings_log_level_ = new QComboBox(form_box);
    settings_log_level_->addItem("Quiet", static_cast<int>(GuiLogLevel::Quiet));
    settings_log_level_->addItem("Normal", static_cast<int>(GuiLogLevel::Normal));
    settings_log_level_->addItem("Verbose", static_cast<int>(GuiLogLevel::Verbose));
    settings_log_level_->setCurrentIndex(1);
    settings_log_level_->setToolTip(
        "Quiet: errors and critical session events only.\n"
        "Normal: ArchStreamer host/client activity (default).\n"
        "Verbose: also logs RetroArch output and enables RetroArch --verbose.");
    form->addRow("Log level", settings_log_level_);

    settings_log_sessions_ = new QSpinBox(form_box);
    settings_log_sessions_->setRange(1, 20);
    settings_log_sessions_->setValue(3);
    settings_log_sessions_->setSuffix(" sessions");
    settings_log_sessions_->setToolTip(
        "How many recent app sessions from gui.log to include when sending logs to the host.");
    settings_send_logs_ = new QPushButton("Send logs to host", form_box);
    settings_send_logs_->setToolTip(
        "Opens a short control connection and uploads recent gui.log sessions "
        "(plus gst video/audio tails when those checkboxes are on) for host-side inspection.");
    form->addRow("Send log depth", settings_log_sessions_);
    form->addRow("", settings_send_logs_);

    auto* debug_box = new QGroupBox("Debug categories", page);
    auto* debug_layout = new QVBoxLayout(debug_box);

    auto add_debug_row = [debug_layout](
                             QCheckBox*& checkbox,
                             QWidget* parent,
                             const QString& title,
                             const QString& on_help,
                             const QString& off_help) {
        checkbox = new QCheckBox(title, parent);
        checkbox->setChecked(false);
        checkbox->setToolTip(off_help + "\n\nWhen on: " + on_help);
        auto* help = new QLabel(off_help, parent);
        help->setWordWrap(true);
        help->setStyleSheet(QStringLiteral("color: palette(mid);"));
        debug_layout->addWidget(checkbox);
        debug_layout->addWidget(help);
        QObject::connect(checkbox, &QCheckBox::toggled, help, [help, on_help, off_help](bool on) {
            help->setText(on ? on_help : off_help);
        });
    };

    add_debug_row(
        logs_controls_,
        debug_box,
        "Log controls",
        "On — pad, keyboard, and mapped control changes are written to gui.log "
        "(lines prefixed ctrl:). Use Send logs to host after a play session.",
        "Off — no per-control spam. Enable to diagnose dead buttons / keyboard vs pad merge.");
    add_debug_row(
        logs_connections_,
        debug_box,
        "Log connections",
        "On — TCP connect/close and session lifecycle are written to gui.log "
        "(lines prefixed conn:). Use Send logs to host after a play session.",
        "Off — no connection lifecycle lines. Enable to diagnose drops, rebinds, and join ordering.");
    add_debug_row(
        logs_video_,
        debug_box,
        "Log video",
        "On — per-heartbeat decode stats, cutovers, and stalls are written to gui.log "
        "(lines prefixed video:). Send logs also attaches gst-video-receiver.log.",
        "Off — no video decode spam. Enable when diagnosing freezes, black screens, or cutovers.");
    add_debug_row(
        logs_audio_,
        debug_box,
        "Log audio",
        "On — audio restart / dead-receiver notes are written to gui.log "
        "(lines prefixed audio:). Send logs also attaches gst-audio-receiver.log.",
        "Off — no audio pipeline spam. Enable when diagnosing lip-sync or silent audio.");

    connect(settings_log_level_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        apply_log_level_from_settings();
        persist_settings_if_idle();
    });
    connect(settings_log_sessions_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        persist_settings_if_idle();
    });
    connect(settings_send_logs_, &QPushButton::clicked, this, [this] {
        send_client_logs_to_host();
    });
    const auto persist_flags = [this](bool) {
        apply_debug_log_flags_from_ui();
        persist_settings_if_idle();
    };
    connect(logs_controls_, &QCheckBox::toggled, this, persist_flags);
    connect(logs_connections_, &QCheckBox::toggled, this, persist_flags);
    connect(logs_video_, &QCheckBox::toggled, this, persist_flags);
    connect(logs_audio_, &QCheckBox::toggled, this, persist_flags);

    auto* left = new QVBoxLayout();
    left->addWidget(form_box);
    left->addWidget(debug_box);
    left->addWidget(new QLabel(
        "Debug categories write to the same gui.log used by Send logs.\n"
        "Leave them off unless diagnosing a session — then enable, reproduce, and send.",
        page));
    left->addStretch();

    logs_log_ = new QPlainTextEdit(page);
    logs_log_->setObjectName(QStringLiteral("logsLog"));
    logs_log_->setReadOnly(true);
    root->addLayout(left, 1);
    root->addWidget(logs_log_, 2);
    return page;
}

QWidget* MainWindow::build_settings_tab() {
    auto* page = new QWidget(this);
    auto* root = new QHBoxLayout(page);

    auto* form_box = new QGroupBox("Local configuration", page);
    auto* form = new QFormLayout(form_box);

    settings_art_root_ = new QLineEdit(form_box);
    settings_art_root_->setPlaceholderText(QStringLiteral("…/ROMS/Art  (under your Gaming root)"));
#ifdef ARCHSTREAMER_HAS_HOST
    host_rom_root_ = new QLineEdit(form_box);
    host_rom_root_->setPlaceholderText(QStringLiteral("…/ROMS/Games  (under your Gaming root)"));
    host_meta_root_ = new QLineEdit(form_box);
    host_meta_root_->setPlaceholderText(QStringLiteral("…/ROMS/Meta  (under your Gaming root)"));
    const auto default_save_root =
        QString::fromStdString(archstreamer::default_save_profile_root().string());
    host_save_root_ = new QLineEdit(default_save_root, form_box);
    host_save_root_->setToolTip(
        "Directory where client usernames store saves, states, and emulator profiles.\n"
        "Layout: <save-root>/<username>/…\n"
        "Flatpak: path must be visible to this app (home is allowed; other disks need\n"
        "flatpak override --filesystem=<path>:rw). Host sessions use the native host_runner\n"
        "path on the host OS.");
    host_save_root_browse_ = new QPushButton("Browse…", form_box);
    host_save_root_create_ = new QPushButton("Create", form_box);
    host_save_root_create_->setToolTip(
        "Create this directory (and parents) if it is missing and writable.");
    host_save_root_status_ = new QLabel(form_box);
    host_save_root_status_->setWordWrap(true);
    host_save_root_status_->setStyleSheet(QStringLiteral("color: #a33;"));
    auto* save_root_row = new QWidget(form_box);
    auto* save_root_layout = new QHBoxLayout(save_root_row);
    save_root_layout->setContentsMargins(0, 0, 0, 0);
    save_root_layout->addWidget(host_save_root_, 1);
    save_root_layout->addWidget(host_save_root_browse_);
    save_root_layout->addWidget(host_save_root_create_);
#endif

    form->addRow("Art root", settings_art_root_);
#ifdef ARCHSTREAMER_HAS_HOST
    form->addRow("ROM root", host_rom_root_);
    form->addRow("Meta root", host_meta_root_);
    form->addRow("Client save root", save_root_row);
    form->addRow("", host_save_root_status_);
#endif

#ifdef ARCHSTREAMER_HAS_HOST
    settings_native_host_runner_ = new QLineEdit(form_box);
    settings_native_host_runner_->setPlaceholderText(
        "auto (ARCHSTREAMER_HOST_RUNNER or common paths)");
    settings_native_host_runner_->setToolTip(
        "When running as a Flatpak, Host start uses flatpak-spawn --host on this binary.\n"
        "Point it at a native host_runner built outside the sandbox (gamecope/uinput/Switch).");
    form->addRow("Native host_runner", settings_native_host_runner_);
    connect(settings_native_host_runner_, &QLineEdit::editingFinished, this, [this] {
        persist_settings_if_idle();
    });
#endif

    settings_audio_out_ = new QComboBox(form_box);
    settings_audio_out_->setToolTip(
        "Playback device for Client receive audio and Host Watch stream locally.\n"
        "Change anytime — including mid-session — to try outputs.");
    refresh_settings_audio_outputs();
    form->addRow("Audio output", settings_audio_out_);

    auto* refresh_audio = new QPushButton("Refresh audio devices", form_box);
    form->addRow("", refresh_audio);

    auto* refresh_art = new QPushButton("Refresh Art from Steam", form_box);
    form->addRow("", refresh_art);

    settings_session_timeout_ = new QSpinBox(form_box);
    settings_session_timeout_->setRange(5, 3600);
    settings_session_timeout_->setValue(30);
    settings_session_timeout_->setSuffix(" s");
    settings_session_timeout_->setToolTip(
        "How long the host waits for remote clients to join before giving up.\n"
        "Increase this when testing LAN connections between machines.");

    settings_show_framecount_ = new QCheckBox(
        "Show host Frames counter (debug)",
        form_box);
    settings_show_framecount_->setChecked(false);
    settings_show_framecount_->setToolTip(
        "Asks the host to overlay a ticking Frames: counter on the RetroArch stream.\n"
        "Default off. Can be toggled while connected (sent via session heartbeats).\n"
        "Useful when diagnosing stuck static menus on GL/Xvfb capture.");

    form->addRow("Host lobby wait", settings_session_timeout_);
    form->addRow("", settings_show_framecount_);

#ifdef ARCHSTREAMER_HAS_HOST
    settings_allow_new_users_ = new QCheckBox("Allow new users", form_box);
    settings_allow_new_users_->setChecked(false);
    settings_allow_new_users_->setToolTip(
        "When checked, clients may create a new save profile with a first-time username.\n"
        "When unchecked (default), only existing save-profile users can join.");
    form->addRow("", settings_allow_new_users_);
    connect(settings_allow_new_users_, &QCheckBox::toggled, this, [this](bool) {
        persist_settings_if_idle();
    });
#endif

    connect(settings_art_root_, &QLineEdit::editingFinished, this, [this] {
        apply_art_root_to_pickers();
        persist_settings_if_idle();
    });
    connect(settings_art_root_, &QLineEdit::textChanged, this, [this](const QString&) {
        apply_art_root_to_pickers();
    });
#ifdef ARCHSTREAMER_HAS_HOST
    connect(host_rom_root_, &QLineEdit::editingFinished, this, [this] {
        persist_settings_if_idle();
    });
    connect(host_meta_root_, &QLineEdit::editingFinished, this, [this] {
        persist_settings_if_idle();
    });
    connect(host_save_root_, &QLineEdit::editingFinished, this, [this] {
        update_save_root_status();
        // If the resolved path is a real directory, remember it; otherwise keep typing.
        const auto path = save_root_path();
        if (std::filesystem::is_directory(path)) {
            persist_valid_save_root(path);
        } else {
            persist_settings_if_idle();
        }
    });
    connect(host_save_root_, &QLineEdit::textChanged, this, [this](const QString&) {
        update_save_root_status();
    });
    connect(host_save_root_browse_, &QPushButton::clicked, this, [this] {
        browse_save_root();
    });
    connect(host_save_root_create_, &QPushButton::clicked, this, [this] {
        create_save_root();
    });
    update_save_root_status();
#endif
    connect(settings_session_timeout_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        persist_settings_if_idle();
    });
    connect(settings_show_framecount_, &QCheckBox::toggled, this, [this](bool checked) {
        if (heartbeat_prefs_) {
            heartbeat_prefs_->set_show_framecount(checked);
        }
        persist_settings_if_idle();
    });
    connect(settings_audio_out_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        apply_audio_output_from_settings();
        persist_settings_if_idle();
    });
    connect(refresh_audio, &QPushButton::clicked, this, [this] {
        refresh_settings_audio_outputs();
        if (settings_log_ != nullptr) {
            append_log(settings_log_, "Refreshed audio output device list.");
        }
    });
    connect(refresh_art, &QPushButton::clicked, this, [this] {
        refresh_art_from_steam();
    });

    auto* left = new QVBoxLayout();
    left->addWidget(form_box);
    left->addWidget(new QLabel(
#ifdef ARCHSTREAMER_HAS_HOST
        "Art / ROM / Meta roots are used by the local host and Steam art import.\n"
        "Clients cache host art under ~/.cache/archstreamer/hosts/<host>/Art.\n"
        "Steam account ID is on the Profile tab.\n"
        "Stream quality, capture, and GPU options live on the Stream tab.\n"
        "Log level and Send logs live on the Logs tab.",
#else
        "Art root is used for local Steam import when available.\n"
        "Clients cache host art under the ArchStreamer cache directory.\n"
        "Steam account ID is on the Profile tab.\n"
        "Stream quality options live on the Stream tab.\n"
        "Log level and Send logs live on the Logs tab.",
#endif
        page));
    left->addWidget(build_updates_group(page));
    left->addStretch();

    settings_log_ = new QPlainTextEdit(page);
    settings_log_->setReadOnly(true);
    root->addLayout(left, 1);
    root->addWidget(settings_log_, 2);
    return page;
}

void MainWindow::toggle_pad_on_screen_keyboard(bool open) {
    if (!open) {
        close_pad_on_screen_keyboard();
        return;
    }

    prepare_video_for_pad_osk();

    if (pad_osk_ != nullptr) {
        place_pad_osk_over_video();
        pad_osk_->raise();
        pad_osk_->activateWindow();
        if (game_options_pad_osk_ != nullptr) {
            const QSignalBlocker blocker(game_options_pad_osk_);
            game_options_pad_osk_->setChecked(true);
        }
        return;
    }

    soft_keyboard_request_id_ = 0;
    PadOnScreenKeyboard::Options options;
    options.title = QStringLiteral("Pad on-screen keyboard");
    options.prompt = soft_keyboard_
        ? QStringLiteral("Type text for the host to inject into any open dialog.")
        : QStringLiteral("Test the controller letter keyboard.");
    options.max_length = 12;
    pad_osk_ = make_pad_osk(std::move(options));
    connect(pad_osk_, &QDialog::finished, this, [this](int result) {
        const QString text = pad_osk_ != nullptr ? pad_osk_->text() : QString{};
        pad_osk_ = nullptr;
        if (game_options_pad_osk_ != nullptr) {
            const QSignalBlocker blocker(game_options_pad_osk_);
            game_options_pad_osk_->setChecked(false);
        }
        // Unsolicited inject: request_id 0 tells the host to find a dialog itself.
        if (result == QDialog::Accepted && soft_keyboard_ && !text.trimmed().isEmpty()) {
            SoftKeyboardResponse response;
            response.request_id = 0;
            response.accepted = true;
            response.text = text.trimmed().toStdString();
            soft_keyboard_->submit_response(std::move(response));
            append_log(
                client_log_,
                QStringLiteral("Sent manual pad keyboard text to host."));
        }
        restore_video_window_focus();
    });
    place_pad_osk_over_video();
    pad_osk_->show();
    pad_osk_->raise();
    pad_osk_->activateWindow();
    if (game_options_pad_osk_ != nullptr) {
        const QSignalBlocker blocker(game_options_pad_osk_);
        game_options_pad_osk_->setChecked(true);
    }
}

void MainWindow::open_soft_keyboard_from_host(const SoftKeyboardRequest& request) {
    prepare_video_for_pad_osk();

    // A repeat of the id we are already showing must not rebuild the dialog — that
    // would discard what the player has typed so far.
    if (pad_osk_ != nullptr && soft_keyboard_request_id_ == request.request_id) {
        place_pad_osk_over_video();
        pad_osk_->raise();
        pad_osk_->activateWindow();
        return;
    }

    if (pad_osk_ != nullptr) {
        pad_osk_->disconnect();
        pad_osk_->deleteLater();
        pad_osk_ = nullptr;
    }

    soft_keyboard_request_id_ = request.request_id;
    const auto prompt = request.prompt.empty()
        ? QStringLiteral("The game is asking for text. Enter it with the pad.")
        : QString::fromStdString(request.prompt);

    PadOnScreenKeyboard::Options options;
    options.title = QStringLiteral("Software Keyboard");
    options.prompt = prompt;
    options.initial_text = QString::fromStdString(request.initial_text);
    options.max_length = request.max_length == 0 ? 12 : static_cast<int>(request.max_length);
    pad_osk_ = make_pad_osk(std::move(options));
    connect(pad_osk_, &QDialog::finished, this, [this](int result) {
        const QString text = pad_osk_ != nullptr ? pad_osk_->text() : QString{};
        const auto request_id = soft_keyboard_request_id_;
        pad_osk_ = nullptr;
        soft_keyboard_request_id_ = 0;
        if (game_options_pad_osk_ != nullptr) {
            const QSignalBlocker blocker(game_options_pad_osk_);
            game_options_pad_osk_->setChecked(false);
        }
        if (request_id != 0 && soft_keyboard_) {
            SoftKeyboardResponse response;
            response.request_id = request_id;
            response.accepted = result == QDialog::Accepted;
            response.text = text.toStdString();
            soft_keyboard_->submit_response(std::move(response));
            append_log(
                client_log_,
                response.accepted
                    ? QStringLiteral("Sent pad keyboard text to host.")
                    : QStringLiteral("Cancelled pad keyboard."));
        }
        restore_video_window_focus();
    });
    place_pad_osk_over_video();
    pad_osk_->show();
    pad_osk_->raise();
    pad_osk_->activateWindow();
    if (game_options_pad_osk_ != nullptr) {
        const QSignalBlocker blocker(game_options_pad_osk_);
        game_options_pad_osk_->setChecked(true);
    }
    append_log(client_log_, QStringLiteral("Pad keyboard opened: %1").arg(prompt));
}

PadOnScreenKeyboard* MainWindow::make_pad_osk(PadOnScreenKeyboard::Options options) {
    // Top-level (no MainWindow parent) so it appears over the video surface, not
    // buried with the settings UI on another monitor.
    auto* osk = new PadOnScreenKeyboard(std::move(options), /*parent=*/nullptr);
    osk->setAttribute(Qt::WA_DeleteOnClose);
    osk->setWindowFlags(Qt::Dialog | Qt::WindowStaysOnTopHint);
    return osk;
}

void MainWindow::place_pad_osk_over_video() {
    if (pad_osk_ == nullptr) {
        return;
    }

    QWidget* video = nullptr;
    if (client_video_controller_ != nullptr) {
        video = client_video_controller_->surface();
    }
#ifdef ARCHSTREAMER_HAS_HOST
    if (video == nullptr && host_local_video_controller_ != nullptr) {
        video = host_local_video_controller_->surface();
    }
#endif

    QRect anchor;
    if (video != nullptr && video->isVisible()) {
        anchor = QRect(video->mapToGlobal(QPoint(0, 0)), video->size());
    } else {
        anchor = frameGeometry();
    }

    pad_osk_->adjustSize();
    QRect geo = pad_osk_->frameGeometry();
    geo.moveCenter(anchor.center());

    QScreen* screen = QGuiApplication::screenAt(anchor.center());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen != nullptr) {
        const QRect avail = screen->availableGeometry();
        if (!avail.intersects(geo)) {
            geo.moveCenter(avail.center());
        } else {
            geo.moveLeft(std::clamp(geo.left(), avail.left(), avail.right() - geo.width() + 1));
            geo.moveTop(std::clamp(geo.top(), avail.top(), avail.bottom() - geo.height() + 1));
        }
    }
    pad_osk_->move(geo.topLeft());
}

void MainWindow::prepare_video_for_pad_osk() {
    if (client_video_controller_) {
        client_video_controller_->suspendFullScreenForOverlay();
    }
#ifdef ARCHSTREAMER_HAS_HOST
    if (host_local_video_controller_) {
        host_local_video_controller_->suspendFullScreenForOverlay();
    }
#endif
}

void MainWindow::restore_video_window_focus() {
    if (client_video_controller_) {
        client_video_controller_->resumeFullScreenAfterOverlay();
    }
#ifdef ARCHSTREAMER_HAS_HOST
    if (host_local_video_controller_) {
        host_local_video_controller_->resumeFullScreenAfterOverlay();
    }
#endif
    // Deferred: Qt is still tearing the dialog down and will hand focus back to this
    // window, which would land on top of whatever we raise right now.
    QTimer::singleShot(150, this, [this] {
        if (client_video_controller_) {
            client_video_controller_->raiseVideo();
            return;
        }
#ifdef ARCHSTREAMER_HAS_HOST
        if (host_local_video_controller_) {
            host_local_video_controller_->raiseVideo();
            return;
        }
#endif
        archstreamer::raise_video_window();
    });
}

void MainWindow::close_pad_on_screen_keyboard() {
    if (pad_osk_ != nullptr) {
        pad_osk_->close();
        pad_osk_ = nullptr;
    }
    if (game_options_pad_osk_ != nullptr) {
        const QSignalBlocker blocker(game_options_pad_osk_);
        game_options_pad_osk_->setChecked(false);
    }
    restore_video_window_focus();
}

} // namespace archstreamer::gui
