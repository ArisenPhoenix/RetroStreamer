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

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
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

MainWindow::MainWindow() {
    // Block persistence until widgets exist and load_persisted_settings finishes.
    // Host/client tab construction refreshes combos and would otherwise save defaults
    // (e.g. lobby wait = 30) before the Settings controls are created.
    restoring_settings_ = true;
    setWindowTitle("ArchStreamer");
    resize(1100, 720);

    tabs_ = new QTabWidget(this);
    tabs_->addTab(build_client_tab(), "Client");
#ifdef ARCHSTREAMER_HAS_HOST
    tabs_->addTab(build_host_tab(), "Host");
#endif
    tabs_->addTab(build_stream_tab(), "Stream");
    tabs_->addTab(build_game_options_tab(), "Game Options");
    tabs_->addTab(build_profile_tab(), "Profile");
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
    client_send_keyboard_ = new QCheckBox("Send keyboard (Space=FF, P=pause)", form_box);
    client_send_keyboard_->setChecked(true);
    client_send_keyboard_->setToolTip(
        "Forwards Space, P, arrows, Enter, Esc, Tab, Backspace, and F1 to the host.\n"
        "Space = fast-forward (hold), F8 = Yuzu continuous FF, P = pause, F1 = RetroArch menu.\n"
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
    form->addRow("Role", client_role_);
    form->addRow("Mode", client_mode_);
    form->addRow("Players", client_players_);
    form->addRow("", client_send_keyboard_);

    client_game_picker_ = new archstreamer::gui::GamePickerWidget(page);
    client_game_picker_->setArtRoot(art_root_path());
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

QWidget* MainWindow::build_game_options_tab() {
    auto* page = new QWidget(this);
    auto* root = new QVBoxLayout(page);

    face_button_prefs_ = std::make_shared<archstreamer::ClientFaceButtonPrefs>();

    auto* face_box = new QGroupBox("Face buttons", page);
    auto* face_form = new QFormLayout(face_box);
    game_options_swap_nw_ = new QCheckBox("Swap NW (Triangle ↔ Square / Y ↔ X)", face_box);
    game_options_swap_se_ = new QCheckBox("Swap SE (Cross ↔ Circle / A ↔ B)", face_box);
    game_options_swap_nw_->setToolTip(
        "Swap the north and west face buttons before they reach the host.\n"
        "Use this when Triangle/Square (or Y/X) feel reversed for the current system.");
    game_options_swap_se_->setToolTip(
        "Swap the south and east face buttons before they reach the host.\n"
        "Use this when Cross/Circle (or A/B) feel reversed for the current system.");
    face_form->addRow("", game_options_swap_nw_);
    face_form->addRow("", game_options_swap_se_);
    connect(game_options_swap_nw_, &QCheckBox::toggled, this, [this](bool checked) {
        if (face_button_prefs_) {
            face_button_prefs_->set_swap_nw(checked);
        }
        persist_settings_if_idle();
    });
    connect(game_options_swap_se_, &QCheckBox::toggled, this, [this](bool checked) {
        if (face_button_prefs_) {
            face_button_prefs_->set_swap_se(checked);
        }
        persist_settings_if_idle();
    });
    root->addWidget(face_box);

    game_options_pad_osk_ = new QPushButton(QStringLiteral("Pad on-screen keyboard"), page);
    game_options_pad_osk_->setCheckable(true);
    game_options_pad_osk_->setToolTip(
        QStringLiteral(
            "Opens a controller-friendly letter keyboard.\n"
            "During a Switch session, Done sends the text to the host so it can fill "
            "any open software-keyboard dialog (escape hatch if auto-prompt misses).\n"
            "Cancel closes without sending; click this button again to close."));
    connect(game_options_pad_osk_, &QPushButton::toggled, this, [this](bool checked) {
        toggle_pad_on_screen_keyboard(checked);
    });
    root->addWidget(game_options_pad_osk_);

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
    left->addWidget(new QLabel(
        "Username is used when joining as a client.\n"
        "Host name is what others see in Select Host (LAN advertise).\n"
        "They default to the same value; set them apart if you host and play under different identities.\n"
        "Steam account ID is used for art import (leave blank to auto-detect).",
        page));
    left->addStretch();

    profile_log_ = new QPlainTextEdit(page);
    profile_log_->setReadOnly(true);
    root->addLayout(left, 1);
    root->addWidget(profile_log_, 2);
    return page;
}

QWidget* MainWindow::build_settings_tab() {
    auto* page = new QWidget(this);
    auto* root = new QHBoxLayout(page);

    auto* form_box = new QGroupBox("Local configuration", page);
    auto* form = new QFormLayout(form_box);

    settings_art_root_ = new QLineEdit(archstreamer::DefaultArtRoot, form_box);
#ifdef ARCHSTREAMER_HAS_HOST
    host_rom_root_ = new QLineEdit(archstreamer::DefaultRomRoot, form_box);
    host_meta_root_ = new QLineEdit(archstreamer::DefaultMetaRoot, form_box);
#endif

    form->addRow("Art root", settings_art_root_);
#ifdef ARCHSTREAMER_HAS_HOST
    form->addRow("ROM root", host_rom_root_);
    form->addRow("Meta root", host_meta_root_);
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

    settings_log_level_ = new QComboBox(form_box);
    settings_log_level_->addItem("Quiet", static_cast<int>(GuiLogLevel::Quiet));
    settings_log_level_->addItem("Normal", static_cast<int>(GuiLogLevel::Normal));
    settings_log_level_->addItem("Verbose", static_cast<int>(GuiLogLevel::Verbose));
    settings_log_level_->setCurrentIndex(1);
    settings_log_level_->setToolTip(
        "Quiet: errors and critical session events only.\n"
        "Normal: ArchStreamer host/client activity (default).\n"
        "Verbose: also logs RetroArch output and enables RetroArch --verbose.");

    settings_show_framecount_ = new QCheckBox(
        "Show host Frames counter (debug)",
        form_box);
    settings_show_framecount_->setChecked(false);
    settings_show_framecount_->setToolTip(
        "Asks the host to overlay a ticking Frames: counter on the RetroArch stream.\n"
        "Default off. Can be toggled while connected (sent via session heartbeats).\n"
        "Useful when diagnosing stuck static menus on GL/Xvfb capture.");

    form->addRow("Host lobby wait", settings_session_timeout_);
    form->addRow("Log level", settings_log_level_);
    form->addRow("", settings_show_framecount_);

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
#endif
    connect(settings_session_timeout_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        persist_settings_if_idle();
    });
    connect(settings_log_level_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        apply_log_level_from_settings();
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
        "Log level Verbose is required to see RetroArch console output.",
#else
        "Art root is used for local Steam import when available.\n"
        "Clients cache host art under the ArchStreamer cache directory.\n"
        "Steam account ID is on the Profile tab.\n"
        "Stream quality options live on the Stream tab.\n"
        "Log level Verbose includes extra client diagnostics.",
#endif
        page));
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

    if (pad_osk_ != nullptr) {
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
    pad_osk_ = new PadOnScreenKeyboard(std::move(options), this);
    pad_osk_->setAttribute(Qt::WA_DeleteOnClose);
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
            restore_video_window_focus();
        }
    });
    pad_osk_->show();
    pad_osk_->raise();
    pad_osk_->activateWindow();
    if (game_options_pad_osk_ != nullptr) {
        const QSignalBlocker blocker(game_options_pad_osk_);
        game_options_pad_osk_->setChecked(true);
    }
}

void MainWindow::open_soft_keyboard_from_host(const SoftKeyboardRequest& request) {
    // A repeat of the id we are already showing must not rebuild the dialog — that
    // would discard what the player has typed so far.
    if (pad_osk_ != nullptr && soft_keyboard_request_id_ == request.request_id) {
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
    pad_osk_ = new PadOnScreenKeyboard(std::move(options), this);
    pad_osk_->setAttribute(Qt::WA_DeleteOnClose);
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
        // This dialog appeared over the game unprompted, so hand the screen back.
        restore_video_window_focus();
    });
    pad_osk_->show();
    pad_osk_->raise();
    pad_osk_->activateWindow();
    if (game_options_pad_osk_ != nullptr) {
        const QSignalBlocker blocker(game_options_pad_osk_);
        game_options_pad_osk_->setChecked(true);
    }
    append_log(client_log_, QStringLiteral("Pad keyboard opened: %1").arg(prompt));
}

void MainWindow::restore_video_window_focus() {
    // Deferred: Qt is still tearing the dialog down and will hand focus back to this
    // window, which would land on top of whatever we raise right now.
    QTimer::singleShot(150, this, [] {
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
}

} // namespace archstreamer::gui
