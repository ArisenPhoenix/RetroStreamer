#include "common/catalog_paths.hpp"
#include "common/catalog_presenter.hpp"
#include "client/client_app.hpp"
#include "client/client_media_playback.hpp"
#include "client/game_filter.hpp"
#include "client/audio_playback_device.hpp"
#include "client/remoted_keyboard_source.hpp"
#include "game_picker_widget.hpp"
#include "host_search_dialog.hpp"
#include "common/addresses.hpp"
#include "common/discovery.hpp"
#include "common/game_assets.hpp"
#include "common/platform/paths.hpp"
#include "common/steam_art_import.hpp"
#ifdef ARCHSTREAMER_HAS_HOST
#include "host/game_catalog_scanner.hpp"
#include "host/gpu_select.hpp"
#include "host/host_app_config.hpp"
#include "host/media_capture.hpp"
#include "host/standalone_emulator.hpp"
#endif

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPixmapCache>
#include <QProcess>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int DefaultInputPort = 45454;
constexpr int DefaultVideoPort = 5004;
constexpr int DefaultAudioPort = 6004;

// Forwards lobby-window key holds into GuiFocusRemotedKeyboardSource (evdev is
// primary while the video window has focus; this covers lobby focus).
class RemotedKeyboardEventFilter final : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        (void)watched;
        if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease) {
            return false;
        }
        const auto* key_event = static_cast<const QKeyEvent*>(event);
        if (key_event->isAutoRepeat()) {
            return false;
        }
        const auto bit = archstreamer::remoted_key_bit_from_qt_key(key_event->key());
        if (bit == 0) {
            return false;
        }
        auto& gui_keys = archstreamer::GuiFocusRemotedKeyboardSource::instance();
        auto keys = gui_keys.poll_keys();
        if (event->type() == QEvent::KeyPress) {
            keys |= bit;
        } else {
            keys &= ~bit;
        }
        gui_keys.set_keys(keys);
        return false;
    }
};

std::atomic_bool mirror_gui_logs_to_stdout = false;

enum class GuiLogLevel : int {
    Quiet = 0,
    Normal = 1,
    Verbose = 2,
};

std::atomic<int> gui_log_level{static_cast<int>(GuiLogLevel::Normal)};

std::filesystem::path gui_log_path() {
    const auto dir = std::filesystem::temp_directory_path() / "archstreamer-logs";
    std::filesystem::create_directories(dir);
    return dir / "gui.log";
}

std::ofstream& log_file() {
    static std::ofstream file(gui_log_path(), std::ios::app);
    return file;
}

void write_to_log_file(const std::string& message) {
    auto& f = log_file();
    f << message << '\n';
    f.flush();
}

QString log_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time_t = std::chrono::system_clock::to_time_t(now);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%H:%M:%S", std::localtime(&time_t));
    return QString::fromLatin1(ts);
}

QString mode_name(archstreamer::GameSessionMode mode) {
    return mode == archstreamer::GameSessionMode::SinglePlayer ? "singleplayer" : "multiplayer";
}

bool is_retroarch_log_line(const QString& line) {
    const auto text = line.trimmed();
    if (text.isEmpty()) {
        return false;
    }
    // RetroArch --verbose / libretro cores emit bracketed subsystem tags.
    static const char* const kTags[] = {
        "[INFO]",
        "[WARN]",
        "[ERROR]",
        "[DEBUG]",
        "[VERBOSE]",
        "[libretro",
        "[GLSL]",
        "[Vulkan]",
        "[GLCore]",
        "[Wayland]",
        "[DRM]",
        "[X11]",
        "[PulseAudio]",
        "[ALSA]",
        "[Joypad]",
        "[Config]",
        "[Environ]",
        "[Autoconf]",
        "[Input]",
        "[Audio]",
        "[Video]",
        "[Core]",
        "[Content]",
        "[State]",
        "[SRAM]",
        "[Savestate]",
        "[Playlist]",
        "[Threaded]",
        "[Fonts]",
        "[Menu]",
        "[Overrides]",
        "[Shaders]",
    };
    for (const char* tag : kTags) {
        if (text.contains(QLatin1String(tag))) {
            return true;
        }
    }
    return text.contains(QLatin1String("RetroArch "));
}

void append_log(QPlainTextEdit* log, QString message, GuiLogLevel level = GuiLogLevel::Normal) {
    if (static_cast<int>(level) > gui_log_level.load()) {
        return;
    }
    if (log != nullptr) {
        const auto name = log->objectName();
        if (name == QLatin1String("hostLog") && !message.startsWith("[host]")) {
            message = "[host] " + message;
        } else if (name == QLatin1String("clientLog") && !message.startsWith("[client]")) {
            message = "[client] " + message;
        }
    }
    message = QString("[%1] %2").arg(log_timestamp(), message);
    write_to_log_file(message.toStdString());
    if (mirror_gui_logs_to_stdout.load()) {
        std::cout << message.toStdString() << '\n';
    }
    if (log == nullptr) {
        return;
    }
    if (QThread::currentThread() == log->thread()) {
        log->appendPlainText(message);
        return;
    }
    QMetaObject::invokeMethod(
        log,
        [log, message = std::move(message)] {
            log->appendPlainText(message);
        },
        Qt::QueuedConnection);
}

void append_host_process_log(QPlainTextEdit* log, const QString& line) {
    const auto trimmed = line.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    append_log(
        log,
        trimmed,
        is_retroarch_log_line(trimmed) ? GuiLogLevel::Verbose : GuiLogLevel::Normal);
}

archstreamer::GameSessionMode selected_mode(const QComboBox* combo) {
    return combo->currentIndex() == 1
        ? archstreamer::GameSessionMode::Multiplayer
        : archstreamer::GameSessionMode::SinglePlayer;
}

archstreamer::ClientParticipantRole selected_client_role(const QComboBox* combo) {
    return combo->currentIndex() == 1
        ? archstreamer::ClientParticipantRole::Viewer
        : archstreamer::ClientParticipantRole::Player;
}

#ifdef ARCHSTREAMER_HAS_HOST
bool host_role_is_viewer(const QComboBox* combo) {
    return combo->currentData().toString() == QStringLiteral("viewer");
}

QString host_role_text(const QComboBox* combo) {
    return host_role_is_viewer(combo) ? QStringLiteral("viewer") : QStringLiteral("player");
}

QString host_runner_program() {
    if (qEnvironmentVariableIsSet("ARCHSTREAMER_HOST_RUNNER")) {
        const auto env = qEnvironmentVariable("ARCHSTREAMER_HOST_RUNNER");
        if (!env.isEmpty()) {
            return env;
        }
    }
    const auto app_dir = std::filesystem::path(QCoreApplication::applicationDirPath().toStdString());
    const auto candidates = {
        app_dir / "host_runner",
        app_dir / "host_runner.exe",
        std::filesystem::current_path() / "build" / "host_runner",
        std::filesystem::current_path() / "build" / "Release" / "host_runner.exe",
        std::filesystem::current_path() / "build" / "host_runner.exe",
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return QString::fromStdString(candidate.string());
        }
    }
    return QStringLiteral("./build/host_runner");
}

bool running_inside_flatpak() {
    if (qEnvironmentVariableIsSet("FLATPAK_ID")) {
        return true;
    }
    return std::filesystem::exists("/.flatpak-info");
}

QString resolve_native_host_runner(const QString& configured) {
    if (!configured.trimmed().isEmpty() && QFileInfo::exists(configured.trimmed())) {
        return configured.trimmed();
    }
    if (const auto env = qEnvironmentVariable("ARCHSTREAMER_HOST_RUNNER"); !env.isEmpty()) {
        if (QFileInfo::exists(env)) {
            return env;
        }
    }
    const QString home = QDir::homePath();
    const QStringList candidates = {
        home + QStringLiteral("/.local/bin/host_runner"),
        home + QStringLiteral("/ArchStreamer-src/build-native/host_runner"),
        home + QStringLiteral("/Programming/Mixed/ArchStreamer/build/host_runner"),
        home + QStringLiteral("/src/ArchStreamer/build/host_runner"),
        QStringLiteral("/usr/local/bin/host_runner"),
        QStringLiteral("/usr/bin/host_runner"),
    };
    for (const auto& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    // Ask the host OS (works when ArchStreamer itself is a Flatpak).
    QProcess which;
    which.start(
        QStringLiteral("flatpak-spawn"),
        {QStringLiteral("--host"), QStringLiteral("which"), QStringLiteral("host_runner")});
    if (which.waitForFinished(2000) && which.exitCode() == 0) {
        const auto path = QString::fromLocal8Bit(which.readAllStandardOutput()).trimmed();
        if (!path.isEmpty() && QFileInfo::exists(path)) {
            return path;
        }
    }
    return {};
}
#endif

class MainWindow final : public QMainWindow {
public:
    MainWindow() {
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

    ~MainWindow() override {
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

    void apply_debug_profile(const QString& profile) {
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

private:
    QWidget* build_client_tab() {
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
        client_video_ = new QCheckBox("Receive video", form_box);
        client_video_->setChecked(true);
        client_audio_ = new QCheckBox("Receive audio", form_box);
        client_audio_->setChecked(true);
        client_send_keyboard_ = new QCheckBox("Send keyboard (Space=FF, P=pause)", form_box);
        client_send_keyboard_->setChecked(true);
        client_send_keyboard_->setToolTip(
            "Forwards Space, P, arrows, Enter, Esc, Tab, Backspace, and F1 to the host.\n"
            "Space = fast-forward (hold), P = pause, F1 = RetroArch menu.\n"
            "Works even when the video window has focus (not only this GUI).");
        client_stream_quality_ = new QComboBox(form_box);
        client_stream_quality_->addItem("Auto", static_cast<int>(archstreamer::MediaQualityTier::Auto));
        client_stream_quality_->addItem("Low (800 kbps / 20 fps / 540p)", static_cast<int>(archstreamer::MediaQualityTier::Low));
        client_stream_quality_->addItem("Medium (3.5 Mbps / 30 fps / 720p)", static_cast<int>(archstreamer::MediaQualityTier::Medium));
        client_stream_quality_->addItem("Med-High (8 Mbps / 60 fps / 720p)", static_cast<int>(archstreamer::MediaQualityTier::MediumHigh));
        client_stream_quality_->addItem("High (12 Mbps / 60 fps / 1080p)", static_cast<int>(archstreamer::MediaQualityTier::High));
        client_stream_quality_->addItem("Very-High (25 Mbps / 60 fps / 1080p)", static_cast<int>(archstreamer::MediaQualityTier::VeryHigh));
        client_stream_quality_->setCurrentIndex(0);
        client_stream_quality_->setToolTip(
            "Preferred video tier sent to the host each second.\n"
            "Auto starts at Medium and steps up/down from decode health (~1 Hz heartbeats).\n"
            "Host captures at 1080p; lower tiers downscale. 60fps only if the game renders that fast.\n"
            "Use Medium or Low on Wi‑Fi / weaker laptops; High/Very-High need a strong link.");
        client_synced_av_ = new QCheckBox("Synced A/V (experimental)", form_box);
        client_synced_av_->setChecked(false);
        client_synced_av_->setToolTip(
            "Use one shared-clock GStreamer pipeline for video+audio lip-sync.\n"
            "Applies to the client session and to Host “Watch stream locally”.\n"
            "Leave unchecked to keep the current dual-pipeline receivers.");
        connect(client_synced_av_, &QCheckBox::toggled, this, [this](bool) {
#ifdef ARCHSTREAMER_HAS_HOST
            // Keep local host watch on the same receive path as the client setting.
            if (host_local_media_ != nullptr && host_local_media_->isChecked()) {
                sync_host_local_media();
            }
#endif
            persist_settings_if_idle();
        });
        connect(client_port_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
            update_client_host_summary(client_host_label_);
            persist_settings_if_idle();
        });
        connect(client_input_port_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
            update_client_host_summary(client_host_label_);
            persist_settings_if_idle();
        });
        connect(client_video_, &QCheckBox::toggled, this, [this](bool) {
            persist_settings_if_idle();
        });
        connect(client_audio_, &QCheckBox::toggled, this, [this](bool) {
            persist_settings_if_idle();
        });
        connect(client_send_keyboard_, &QCheckBox::toggled, this, [this](bool) {
            persist_settings_if_idle();
        });
        connect(client_stream_quality_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
            if (heartbeat_prefs_) {
                heartbeat_prefs_->set_wanted_tier(selected_stream_quality());
            }
            persist_settings_if_idle();
        });

        form->addRow("Host", host_row);
        form->addRow("Control port", client_port_);
        form->addRow("Input port", client_input_port_);
        form->addRow("Role", client_role_);
        form->addRow("Mode", client_mode_);
        form->addRow("Players", client_players_);
        form->addRow("Stream quality", client_stream_quality_);
        form->addRow("", client_video_);
        form->addRow("", client_audio_);
        form->addRow("", client_send_keyboard_);
        form->addRow("", client_synced_av_);

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

    QWidget* build_game_options_tab() {
        auto* page = new QWidget(this);
        auto* root = new QVBoxLayout(page);

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
        root->addStretch();
        return page;
    }

    void refresh_game_options_ui() {
        const bool active = disc_control_ != nullptr && [&] {
            std::lock_guard lock(disc_control_->mutex);
            return disc_control_->session_active && disc_control_->disc_labels.size() >= 2;
        }();

        if (game_options_status_ != nullptr) {
            if (!disc_control_ || ![&] {
                    std::lock_guard lock(disc_control_->mutex);
                    return disc_control_->session_active;
                }()) {
                game_options_status_->setText("Join a multi-disc session (.m3u) to swap discs.");
            } else if (!active) {
                game_options_status_->setText("Active session has no multi-disc playlist.");
            } else {
                std::size_t count = 0;
                {
                    std::lock_guard lock(disc_control_->mutex);
                    count = disc_control_->disc_labels.size();
                }
                game_options_status_->setText(
                    QString("Multi-disc session active (%1 discs). Swap when the game asks.")
                        .arg(count));
            }
        }

        if (game_options_disc_ != nullptr) {
            const QSignalBlocker blocker(game_options_disc_);
            const auto previous = game_options_disc_->currentData().toInt();
            game_options_disc_->clear();
            if (active && disc_control_) {
                std::lock_guard lock(disc_control_->mutex);
                for (std::size_t i = 0; i < disc_control_->disc_labels.size(); ++i) {
                    game_options_disc_->addItem(
                        QString("%1: %2")
                            .arg(i + 1)
                            .arg(QString::fromStdString(disc_control_->disc_labels[i])),
                        static_cast<int>(i));
                }
                const auto index = game_options_disc_->findData(previous);
                game_options_disc_->setCurrentIndex(index >= 0 ? index : 0);
            }
            game_options_disc_->setEnabled(active);
        }
        if (game_options_swap_ != nullptr) {
            game_options_swap_->setEnabled(active);
        }
        if (game_options_prev_ != nullptr) {
            game_options_prev_->setEnabled(active);
        }
        if (game_options_next_ != nullptr) {
            game_options_next_->setEnabled(active);
        }
    }

#ifdef ARCHSTREAMER_HAS_HOST
    QWidget* build_host_tab() {
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
        host_clients_->setRange(1, 2);
        host_clients_->setValue(1);
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
#endif // ARCHSTREAMER_HAS_HOST

    QWidget* build_profile_tab() {
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

    QWidget* build_settings_tab() {
        auto* page = new QWidget(this);
        auto* root = new QHBoxLayout(page);

        auto* form_box = new QGroupBox("Local configuration", page);
        auto* form = new QFormLayout(form_box);

        settings_art_root_ = new QLineEdit(archstreamer::DefaultArtRoot, form_box);

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

        form->addRow("Art root (host / local import)", settings_art_root_);
        form->addRow("Host lobby wait", settings_session_timeout_);
        form->addRow("Log level", settings_log_level_);

        settings_show_framecount_ = new QCheckBox(
            "Show host Frames counter (debug)",
            form_box);
        settings_show_framecount_->setChecked(false);
        settings_show_framecount_->setToolTip(
            "Asks the host to overlay a ticking Frames: counter on the RetroArch stream.\n"
            "Default off. Can be toggled while connected (sent via session heartbeats).\n"
            "Useful when diagnosing stuck static menus on GL/Xvfb capture.");
        form->addRow("", settings_show_framecount_);
        connect(settings_show_framecount_, &QCheckBox::toggled, this, [this](bool checked) {
            if (heartbeat_prefs_) {
                heartbeat_prefs_->set_show_framecount(checked);
            }
            persist_settings_if_idle();
        });
#ifdef ARCHSTREAMER_HAS_HOST
        settings_native_host_runner_ = new QLineEdit(form_box);
        settings_native_host_runner_->setPlaceholderText(
            "auto (ARCHSTREAMER_HOST_RUNNER or common paths)");
        settings_native_host_runner_->setToolTip(
            "When running as a Flatpak, Host start uses flatpak-spawn --host on this binary.\n"
            "Point it at a native host_runner built outside the sandbox (gamescope/uinput/Yuzu).");
        form->addRow("Native host_runner", settings_native_host_runner_);
        connect(settings_native_host_runner_, &QLineEdit::editingFinished, this, [this] {
            persist_settings_if_idle();
        });
#endif
#ifdef ARCHSTREAMER_HAS_HOST
        settings_gpu_ = new QComboBox(form_box);
        settings_gpu_->setToolTip(
            "GPU for game render and H.264 encode (normal single-GPU mode).\n"
            "Auto picks the highest-scoring discrete card.\n"
            "Optional: enable Separate render GPU only if you want encode on one card and render on another.");
        form->addRow("Host GPU", settings_gpu_);

        settings_separate_render_gpu_ = new QCheckBox("Separate render GPU", form_box);
        settings_separate_render_gpu_->setChecked(false);
        settings_separate_render_gpu_->setToolTip(
            "Advanced: encode stays on Host GPU above; render uses the second dropdown.\n"
            "Leave unchecked for normal single-GPU setups.");
        form->addRow("", settings_separate_render_gpu_);

        settings_render_gpu_ = new QComboBox(form_box);
        settings_render_gpu_->setToolTip(
            "Render-only GPU when Separate render GPU is checked.\n"
            "Ignored in normal single-GPU mode.");
        settings_render_gpu_->setEnabled(false);
        form->addRow("Render GPU", settings_render_gpu_);
        refresh_settings_gpus();

        settings_renderer_ = new QComboBox(form_box);
        settings_renderer_->addItem("Auto", QStringLiteral("auto"));
        settings_renderer_->addItem("OpenGL", QStringLiteral("opengl"));
        settings_renderer_->addItem("Vulkan", QStringLiteral("vulkan"));
        settings_renderer_->setCurrentIndex(0);
        settings_renderer_->setToolTip(
            "Preferred graphics API for standalone emulators (Yuzu).\n"
            "Auto: Vulkan on gamescope, OpenGL on VirtualGL.\n"
            "Ignored for RetroArch cores.");
        form->addRow("Standalone renderer", settings_renderer_);

        settings_yuzu_scale_ = new QComboBox(form_box);
        settings_yuzu_scale_->addItem("1x native", 1);
        settings_yuzu_scale_->addItem("2x native", 2);
        settings_yuzu_scale_->addItem("3x native", 3);
        settings_yuzu_scale_->addItem("4x native", 4);
        settings_yuzu_scale_->addItem("5x native", 5);
        settings_yuzu_scale_->addItem("6x native", 6);
        settings_yuzu_scale_->setCurrentIndex(0);
        settings_yuzu_scale_->setToolTip(
            "Yuzu internal resolution scale (docked native ≈ 1080p).\n"
            "2x/3x supersamples into the stream capture — sharper image, more GPU load.\n"
            "Applied to the per-user Yuzu profile on Host start. Ignored for RetroArch.");
        form->addRow("Yuzu resolution", settings_yuzu_scale_);

        settings_retroarch_scale_ = new QComboBox(form_box);
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
            "Written to that core's .opt on Host start. Ignored for Yuzu / other cores.");
        form->addRow("RetroArch resolution", settings_retroarch_scale_);
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

        connect(settings_art_root_, &QLineEdit::editingFinished, this, [this] {
            apply_art_root_to_pickers();
            persist_settings_if_idle();
        });
        connect(settings_art_root_, &QLineEdit::textChanged, this, [this](const QString&) {
            apply_art_root_to_pickers();
        });
        connect(settings_session_timeout_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
            persist_settings_if_idle();
        });
        connect(settings_log_level_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
            apply_log_level_from_settings();
            persist_settings_if_idle();
        });
#ifdef ARCHSTREAMER_HAS_HOST
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
        connect(settings_yuzu_scale_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
            persist_settings_if_idle();
        });
        connect(settings_retroarch_scale_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
            persist_settings_if_idle();
        });
#endif
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
            "Art root is for host-side artwork and Steam import.\n"
            "Clients cache host art under ~/.cache/archstreamer/hosts/<host>/Art.\n"
            "Steam account ID is on the Profile tab.\n"
            "Host lobby wait is how long the host keeps accepting remote joins.\n"
            "Log level Verbose is required to see RetroArch console output.",
#else
            "Art root is used for local Steam import when available.\n"
            "Clients cache host art under the ArchStreamer cache directory.\n"
            "Steam account ID is on the Profile tab.\n"
            "Host lobby wait is unused in this client-only build.\n"
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

    void refresh_client_controllers() {
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

#ifdef ARCHSTREAMER_HAS_HOST
    void populate_gpu_combo(QComboBox* combo, const QString& previous) {
        if (combo == nullptr) {
            return;
        }
        const QSignalBlocker blocker(combo);
        combo->clear();
        combo->addItem("Auto (most performant)", QStringLiteral("auto"));
        try {
            const auto devices = archstreamer::list_render_gpus();
            for (const auto& device : devices) {
                QString label = QString::fromStdString(device.name);
                if (device.memory_mib > 0) {
                    label += QString(" (%1 MiB)").arg(device.memory_mib);
                }
                label += QString(" [%1]").arg(QString::fromStdString(device.id));
                combo->addItem(label, QString::fromStdString(device.id));
            }
            if (!devices.empty()) {
                const auto& preferred = archstreamer::preferred_render_gpu(devices);
                combo->setItemText(
                    0,
                    QString("Auto → %1 [%2]")
                        .arg(QString::fromStdString(preferred.name))
                        .arg(QString::fromStdString(preferred.id)));
            }
        } catch (const std::exception& error) {
            if (settings_log_ != nullptr) {
                append_log(
                    settings_log_,
                    QString("GPU scan failed: %1").arg(error.what()),
                    GuiLogLevel::Quiet);
            }
        }
        const auto index = combo->findData(previous.isEmpty() ? QStringLiteral("auto") : previous);
        combo->setCurrentIndex(index >= 0 ? index : 0);
    }

    void refresh_settings_gpus() {
        const auto encode_prev =
            settings_gpu_ != nullptr ? settings_gpu_->currentData().toString() : QStringLiteral("auto");
        const auto render_prev = settings_render_gpu_ != nullptr
            ? settings_render_gpu_->currentData().toString()
            : QStringLiteral("auto");
        populate_gpu_combo(settings_gpu_, encode_prev);
        populate_gpu_combo(settings_render_gpu_, render_prev);
        update_separate_render_gpu_visibility();
    }

    void update_separate_render_gpu_visibility() {
        if (settings_separate_render_gpu_ == nullptr) {
            return;
        }
        int discrete = 0;
        try {
            for (const auto& device : archstreamer::list_render_gpus()) {
                if (device.nvidia_index >= 0 || device.id.rfind("mesa:", 0) == 0) {
                    ++discrete;
                }
            }
        } catch (...) {
            discrete = 0;
        }
        const bool can_split = discrete >= 2;
        settings_separate_render_gpu_->setVisible(can_split);
        if (!can_split && settings_separate_render_gpu_->isChecked()) {
            const QSignalBlocker blocker(settings_separate_render_gpu_);
            settings_separate_render_gpu_->setChecked(false);
        }
        const bool show_render =
            can_split && settings_separate_render_gpu_->isChecked();
        if (settings_render_gpu_ != nullptr) {
            settings_render_gpu_->setVisible(show_render);
            settings_render_gpu_->setEnabled(show_render);
            if (auto* form = qobject_cast<QFormLayout*>(settings_render_gpu_->parentWidget()->layout())) {
                if (QWidget* label = form->labelForField(settings_render_gpu_)) {
                    label->setVisible(show_render);
                }
            }
        }
    }

    std::string selected_encode_gpu_id() const {
        if (settings_gpu_ == nullptr || settings_gpu_->currentData().isNull()) {
            return "auto";
        }
        const auto id = settings_gpu_->currentData().toString().trimmed();
        return id.isEmpty() ? std::string("auto") : id.toStdString();
    }

    std::string selected_render_gpu_id() const {
        if (settings_render_gpu_ == nullptr || settings_render_gpu_->currentData().isNull()) {
            return "auto";
        }
        const auto id = settings_render_gpu_->currentData().toString().trimmed();
        return id.isEmpty() ? std::string("auto") : id.toStdString();
    }

    QString selected_graphics_api_id() const {
        if (settings_renderer_ == nullptr || settings_renderer_->currentData().isNull()) {
            return QStringLiteral("auto");
        }
        const auto id = settings_renderer_->currentData().toString().trimmed();
        return id.isEmpty() ? QStringLiteral("auto") : id;
    }

    int selected_yuzu_resolution_scale() const {
        if (settings_yuzu_scale_ == nullptr || settings_yuzu_scale_->currentData().isNull()) {
            return 1;
        }
        return qBound(settings_yuzu_scale_->currentData().toInt(), 1, 6);
    }

    int selected_retroarch_resolution_scale() const {
        if (settings_retroarch_scale_ == nullptr || settings_retroarch_scale_->currentData().isNull()) {
            return 1;
        }
        return qBound(settings_retroarch_scale_->currentData().toInt(), 1, 6);
    }
#endif

    void refresh_settings_audio_outputs(const QString& select_id = {}) {
        if (settings_audio_out_ == nullptr) {
            return;
        }
        const QSignalBlocker blocker(settings_audio_out_);
        const auto previous = !select_id.isEmpty()
            ? select_id
            : settings_audio_out_->currentData().toString();
        settings_audio_out_->clear();
        try {
            const auto devices = archstreamer::list_audio_output_devices();
            for (const auto& device : devices) {
                QString label = QString::fromStdString(device.name);
                if (device.id != "auto") {
                    label += QString(" [%1]").arg(QString::fromStdString(device.id));
                }
                settings_audio_out_->addItem(label, QString::fromStdString(device.id));
            }
        } catch (const std::exception& error) {
            settings_audio_out_->addItem("Auto (system default)", QStringLiteral("auto"));
            if (settings_log_ != nullptr) {
                append_log(
                    settings_log_,
                    QString("Audio device scan failed: %1").arg(error.what()),
                    GuiLogLevel::Quiet);
            }
        }
        if (settings_audio_out_->count() == 0) {
            settings_audio_out_->addItem("Auto (system default)", QStringLiteral("auto"));
        }
        const auto index = settings_audio_out_->findData(
            previous.isEmpty() ? QStringLiteral("auto") : previous);
        settings_audio_out_->setCurrentIndex(index >= 0 ? index : 0);
        apply_audio_output_from_settings();
    }

    void apply_audio_output_from_settings() {
        archstreamer::set_preferred_audio_output_device(selected_audio_output_id());
    }

    std::string selected_audio_output_id() const {
        if (settings_audio_out_ == nullptr || settings_audio_out_->currentData().isNull()) {
            return "auto";
        }
        const auto id = settings_audio_out_->currentData().toString().trimmed();
        return id.isEmpty() ? std::string("auto") : id.toStdString();
    }

#ifdef ARCHSTREAMER_HAS_HOST
    void refresh_host_controllers() {
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

    void sync_host_role_and_bridge() {
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

    void sync_host_advertise(bool enabled) {
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
            append_log(host_log_, "LAN advertise broadcasting (UDP 45550).");
        } else {
            host_advertise_timer_->stop();
            host_announcer_.reset();
            append_log(host_log_, "LAN advertise stopped.");
        }
    }

    void advertise_host() {
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

    void load_host_games() {
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
                            "visible to this app, or Yuzu for Switch).")
                        .arg(QString::fromStdString(rom_root.string())),
                    GuiLogLevel::Quiet);
            }
            if (!archstreamer::yuzu_runtime_available()) {
                append_log(
                    host_log_,
                    QString::fromStdString(archstreamer::yuzu_unavailable_message()),
                    GuiLogLevel::Quiet);
            }
        } catch (const std::exception& error) {
            host_game_picker_->setCatalog({});
            host_status_->setText("Host stopped; game load failed");
            append_log(host_log_, QString("Load games failed: %1").arg(error.what()), GuiLogLevel::Quiet);
        }
    }
#endif // ARCHSTREAMER_HAS_HOST

    void load_persisted_settings() {
        restoring_settings_ = true;
        QSettings settings("ArchStreamer", "ArchStreamer");
        const auto art_root = settings.value("paths/artRoot", archstreamer::DefaultArtRoot).toString();
        const auto account = settings.value("steam/accountId").toString().trimmed();
        const auto session_timeout = settings.value("host/sessionTimeoutSeconds", 30).toInt();
        const auto log_level = settings.value("ui/logLevel", static_cast<int>(GuiLogLevel::Normal)).toInt();
        auto username = settings.value("profile/username").toString().trimmed();
        if (username.isEmpty()) {
            username = QString::fromStdString(archstreamer::current_username());
            if (username.isEmpty()) {
                username = QStringLiteral("local");
            }
        }
        auto host_name = settings.value("profile/hostName").toString().trimmed();
        if (host_name.isEmpty()) {
            host_name = username;
        }
        if (profile_username_ != nullptr) {
            profile_username_->setText(username);
        }
        if (profile_host_name_ != nullptr) {
            profile_host_name_->setText(host_name);
        }
        if (settings_art_root_ != nullptr) {
            settings_art_root_->setText(art_root);
        }
        if (profile_steam_account_ != nullptr) {
            profile_steam_account_->setText(account);
        }
        if (settings_session_timeout_ != nullptr) {
            const QSignalBlocker blocker(settings_session_timeout_);
            settings_session_timeout_->setValue(qBound(session_timeout, 5, 3600));
        }
        if (settings_log_level_ != nullptr) {
            const auto index = settings_log_level_->findData(qBound(log_level, 0, 2));
            const QSignalBlocker blocker(settings_log_level_);
            settings_log_level_->setCurrentIndex(index >= 0 ? index : 1);
        }
#ifdef ARCHSTREAMER_HAS_HOST
        if (settings_native_host_runner_ != nullptr) {
            const QSignalBlocker blocker(settings_native_host_runner_);
            settings_native_host_runner_->setText(
                settings.value("host/nativeHostRunner").toString());
        }
#endif
#ifdef ARCHSTREAMER_HAS_HOST
        if (settings_gpu_ != nullptr) {
            const auto gpu_id = settings.value(
                "graphics/encodeGpuId",
                settings.value("graphics/gpuId", "auto")).toString();
            const QSignalBlocker blocker(settings_gpu_);
            const auto index = settings_gpu_->findData(gpu_id);
            settings_gpu_->setCurrentIndex(index >= 0 ? index : 0);
            if (index < 0 && gpu_id != "auto" && settings_log_ != nullptr) {
                append_log(
                    settings_log_,
                    QString("Saved encode GPU '%1' is unavailable; using Auto.").arg(gpu_id));
            }
        }
        if (settings_separate_render_gpu_ != nullptr) {
            const QSignalBlocker blocker(settings_separate_render_gpu_);
            settings_separate_render_gpu_->setChecked(
                settings.value("graphics/separateRenderGpu", false).toBool());
        }
        if (settings_render_gpu_ != nullptr) {
            const auto gpu_id = settings.value("graphics/renderGpuId", "auto").toString();
            const QSignalBlocker blocker(settings_render_gpu_);
            const auto index = settings_render_gpu_->findData(gpu_id);
            settings_render_gpu_->setCurrentIndex(index >= 0 ? index : 0);
            if (index < 0 && gpu_id != "auto" && settings_log_ != nullptr) {
                append_log(
                    settings_log_,
                    QString("Saved render GPU '%1' is unavailable; using Auto.").arg(gpu_id));
            }
        }
        update_separate_render_gpu_visibility();
        if (settings_renderer_ != nullptr) {
            const auto renderer = settings.value("graphics/renderer", "auto").toString();
            const QSignalBlocker blocker(settings_renderer_);
            const auto index = settings_renderer_->findData(renderer);
            settings_renderer_->setCurrentIndex(index >= 0 ? index : 0);
        }
        if (settings_yuzu_scale_ != nullptr) {
            const auto scale = qBound(settings.value("graphics/yuzuResolutionScale", 1).toInt(), 1, 6);
            const QSignalBlocker blocker(settings_yuzu_scale_);
            const auto index = settings_yuzu_scale_->findData(scale);
            settings_yuzu_scale_->setCurrentIndex(index >= 0 ? index : 0);
        }
        if (settings_retroarch_scale_ != nullptr) {
            const auto scale =
                qBound(settings.value("graphics/retroarchResolutionScale", 1).toInt(), 1, 6);
            const QSignalBlocker blocker(settings_retroarch_scale_);
            const auto index = settings_retroarch_scale_->findData(scale);
            settings_retroarch_scale_->setCurrentIndex(index >= 0 ? index : 0);
        }
#endif
        if (settings_audio_out_ != nullptr) {
            const auto audio_id = settings.value("audio/outputDevice", "auto").toString();
            refresh_settings_audio_outputs(audio_id);
            if (settings_audio_out_->currentData().toString() != audio_id &&
                audio_id != "auto" &&
                settings_log_ != nullptr) {
                append_log(
                    settings_log_,
                    QString("Saved audio output '%1' is unavailable; using Auto/default.")
                        .arg(audio_id));
            }
        }

        if (client_host_ != nullptr) {
            const auto address = settings.value("client/hostAddress").toString().trimmed();
            client_host_label_ = settings.value("client/hostLabel").toString().trimmed();
            if (client_port_ != nullptr) {
                client_port_->setValue(qBound(settings.value("client/controlPort", 45555).toInt(), 1, 65535));
            }
            if (client_input_port_ != nullptr) {
                client_input_port_->setValue(
                    qBound(settings.value("client/inputPort", DefaultInputPort).toInt(), 1, 65535));
            }
            client_host_->setText(address);
            update_client_host_summary(client_host_label_);
        }
        if (client_role_ != nullptr) {
            const auto role = settings.value("client/role", "Player").toString();
            const auto index = client_role_->findText(role);
            client_role_->setCurrentIndex(index >= 0 ? index : 0);
        }
        if (client_mode_ != nullptr) {
            const auto mode = settings.value("client/mode", "Singleplayer").toString();
            const auto index = client_mode_->findText(mode);
            client_mode_->setCurrentIndex(index >= 0 ? index : 0);
        }
        if (client_players_ != nullptr) {
            client_players_->setValue(qBound(settings.value("client/players", 1).toInt(), 0, 2));
        }
        if (client_video_ != nullptr) {
            client_video_->setChecked(settings.value("client/receiveVideo", true).toBool());
        }
        if (client_audio_ != nullptr) {
            client_audio_->setChecked(settings.value("client/receiveAudio", true).toBool());
        }
        if (client_send_keyboard_ != nullptr) {
            client_send_keyboard_->setChecked(settings.value("client/sendKeyboard", true).toBool());
        }
        if (client_synced_av_ != nullptr) {
            client_synced_av_->setChecked(settings.value("client/syncedAv", false).toBool());
        }
        if (client_stream_quality_ != nullptr) {
            const auto tier = settings.value(
                "client/streamQuality",
                static_cast<int>(archstreamer::MediaQualityTier::Auto)).toInt();
            const QSignalBlocker blocker(client_stream_quality_);
            const auto index = client_stream_quality_->findData(tier);
            client_stream_quality_->setCurrentIndex(index >= 0 ? index : 0);
        }
        if (settings_show_framecount_ != nullptr) {
            const QSignalBlocker blocker(settings_show_framecount_);
            settings_show_framecount_->setChecked(
                settings.value("client/showFramecount", false).toBool());
        }

#ifdef ARCHSTREAMER_HAS_HOST
        if (host_rom_root_ != nullptr) {
            host_rom_root_->setText(
                settings.value("host/romRoot", archstreamer::DefaultRomRoot).toString());
        }
        if (host_meta_root_ != nullptr) {
            host_meta_root_->setText(
                settings.value("host/metaRoot", archstreamer::DefaultMetaRoot).toString());
        }
        if (host_control_port_ != nullptr) {
            host_control_port_->setValue(
                qBound(settings.value("host/controlPort", 45555).toInt(), 1, 65535));
        }
        if (host_input_port_ != nullptr) {
            host_input_port_->setValue(
                qBound(settings.value("host/inputPort", DefaultInputPort).toInt(), 1, 65535));
        }
        if (host_video_port_ != nullptr) {
            host_video_port_->setValue(
                qBound(settings.value("host/videoPort", DefaultVideoPort).toInt(), 1, 65535));
        }
        if (host_audio_port_ != nullptr) {
            host_audio_port_->setValue(
                qBound(settings.value("host/audioPort", DefaultAudioPort).toInt(), 1, 65535));
        }
        if (host_clients_ != nullptr) {
            host_clients_->setValue(qBound(settings.value("host/maxClients", 1).toInt(), 1, 2));
        }
        if (host_role_ != nullptr) {
            const auto role = settings.value("host/role", "viewer").toString().toLower();
            const auto index = host_role_->findData(role);
            host_role_->setCurrentIndex(index >= 0 ? index : 0);
            sync_host_role_and_bridge();
        }
        if (host_mode_ != nullptr) {
            const auto mode = settings.value("host/mode", "Singleplayer").toString();
            const auto index = host_mode_->findText(mode);
            host_mode_->setCurrentIndex(index >= 0 ? index : 0);
        }
        if (host_video_ != nullptr) {
            host_video_->setChecked(settings.value("host/streamVideo", true).toBool());
        }
        if (host_audio_ != nullptr) {
            host_audio_->setChecked(settings.value("host/streamAudio", true).toBool());
        }
        if (host_local_media_ != nullptr) {
            host_local_media_->setChecked(settings.value("host/watchLocally", false).toBool());
        }
        if (host_advertise_ != nullptr) {
            host_advertise_->setChecked(settings.value("host/advertiseLan", true).toBool());
        }
        persisted_host_game_id_ = settings.value("host/lastGameId").toString().trimmed();
#endif
        persisted_client_game_id_ = settings.value("client/lastGameId").toString().trimmed();

        apply_log_level_from_settings();
        apply_art_root_to_pickers();
        if (!account.isEmpty() && profile_log_ != nullptr) {
            append_log(profile_log_, QString("Loaded Steam account ID %1").arg(account));
        }
        restoring_settings_ = false;
    }

    void save_persisted_settings() {
        if (restoring_settings_) {
            return;
        }
        QSettings settings("ArchStreamer", "ArchStreamer");
        settings.setValue("paths/artRoot", QString::fromStdString(art_root_path().string()));
        settings.setValue("steam/accountId", QString::fromStdString(steam_account_id_text()));
        settings.setValue("profile/username", QString::fromStdString(profile_client_username()));
        settings.setValue("profile/hostName", QString::fromStdString(profile_host_name()));
        // Only write when the Settings control exists — early construction saves used to
        // fall back to 30 and wipe a previously persisted lobby wait.
        if (settings_session_timeout_ != nullptr) {
            settings.setValue("host/sessionTimeoutSeconds", settings_session_timeout_->value());
        }
        settings.setValue("ui/logLevel", static_cast<int>(current_log_level()));
#ifdef ARCHSTREAMER_HAS_HOST
        if (settings_native_host_runner_ != nullptr) {
            settings.setValue("host/nativeHostRunner", settings_native_host_runner_->text().trimmed());
        }
#endif
#ifdef ARCHSTREAMER_HAS_HOST
        if (settings_gpu_ != nullptr) {
            const auto encode_id = QString::fromStdString(selected_encode_gpu_id());
            settings.setValue("graphics/encodeGpuId", encode_id);
            settings.setValue("graphics/gpuId", encode_id); // legacy key
        }
        if (settings_separate_render_gpu_ != nullptr) {
            settings.setValue(
                "graphics/separateRenderGpu",
                settings_separate_render_gpu_->isChecked());
        }
        if (settings_render_gpu_ != nullptr) {
            settings.setValue("graphics/renderGpuId", QString::fromStdString(selected_render_gpu_id()));
        }
        if (settings_renderer_ != nullptr) {
            settings.setValue("graphics/renderer", selected_graphics_api_id());
        }
        if (settings_yuzu_scale_ != nullptr) {
            settings.setValue("graphics/yuzuResolutionScale", selected_yuzu_resolution_scale());
        }
        if (settings_retroarch_scale_ != nullptr) {
            settings.setValue(
                "graphics/retroarchResolutionScale",
                selected_retroarch_resolution_scale());
        }
#endif
        if (settings_audio_out_ != nullptr) {
            settings.setValue("audio/outputDevice", QString::fromStdString(selected_audio_output_id()));
        }

        if (client_host_ != nullptr) {
            settings.setValue("client/hostAddress", client_host_->text().trimmed());
        }
        settings.setValue("client/hostLabel", client_host_label_);
        if (client_port_ != nullptr) {
            settings.setValue("client/controlPort", client_port_->value());
        }
        if (client_input_port_ != nullptr) {
            settings.setValue("client/inputPort", client_input_port_->value());
        }
        if (client_role_ != nullptr) {
            settings.setValue("client/role", client_role_->currentText());
        }
        if (client_mode_ != nullptr) {
            settings.setValue("client/mode", client_mode_->currentText());
        }
        if (client_players_ != nullptr) {
            settings.setValue("client/players", client_players_->value());
        }
        if (client_video_ != nullptr) {
            settings.setValue("client/receiveVideo", client_video_->isChecked());
        }
        if (client_audio_ != nullptr) {
            settings.setValue("client/receiveAudio", client_audio_->isChecked());
        }
        if (client_send_keyboard_ != nullptr) {
            settings.setValue("client/sendKeyboard", client_send_keyboard_->isChecked());
        }
        if (client_synced_av_ != nullptr) {
            settings.setValue("client/syncedAv", client_synced_av_->isChecked());
        }
        if (client_stream_quality_ != nullptr) {
            settings.setValue("client/streamQuality", client_stream_quality_->currentData().toInt());
        }
        if (settings_show_framecount_ != nullptr) {
            settings.setValue("client/showFramecount", settings_show_framecount_->isChecked());
        }
        if (client_game_picker_ != nullptr && client_game_picker_->hasSelection()) {
            persisted_client_game_id_ =
                QString::fromStdString(*client_game_picker_->selectedGameId());
            settings.setValue("client/lastGameId", persisted_client_game_id_);
        } else if (!persisted_client_game_id_.isEmpty()) {
            settings.setValue("client/lastGameId", persisted_client_game_id_);
        }

#ifdef ARCHSTREAMER_HAS_HOST
        if (host_rom_root_ != nullptr) {
            settings.setValue("host/romRoot", host_rom_root_->text().trimmed());
        }
        if (host_meta_root_ != nullptr) {
            settings.setValue("host/metaRoot", host_meta_root_->text().trimmed());
        }
        if (host_control_port_ != nullptr) {
            settings.setValue("host/controlPort", host_control_port_->value());
        }
        if (host_input_port_ != nullptr) {
            settings.setValue("host/inputPort", host_input_port_->value());
        }
        if (host_video_port_ != nullptr) {
            settings.setValue("host/videoPort", host_video_port_->value());
        }
        if (host_audio_port_ != nullptr) {
            settings.setValue("host/audioPort", host_audio_port_->value());
        }
        if (host_clients_ != nullptr) {
            settings.setValue("host/maxClients", host_clients_->value());
        }
        if (host_role_ != nullptr) {
            settings.setValue("host/role", host_role_->currentData().toString());
        }
        if (host_mode_ != nullptr) {
            settings.setValue("host/mode", host_mode_->currentText());
        }
        if (host_video_ != nullptr) {
            settings.setValue("host/streamVideo", host_video_->isChecked());
        }
        if (host_audio_ != nullptr) {
            settings.setValue("host/streamAudio", host_audio_->isChecked());
        }
        if (host_local_media_ != nullptr) {
            settings.setValue("host/watchLocally", host_local_media_->isChecked());
        }
        if (host_advertise_ != nullptr) {
            settings.setValue("host/advertiseLan", host_advertise_->isChecked());
        }
        if (host_game_picker_ != nullptr && host_game_picker_->hasSelection()) {
            persisted_host_game_id_ =
                QString::fromStdString(*host_game_picker_->selectedGameId());
            settings.setValue("host/lastGameId", persisted_host_game_id_);
        } else if (!persisted_host_game_id_.isEmpty()) {
            settings.setValue("host/lastGameId", persisted_host_game_id_);
        }
#endif
    }

    void persist_settings_if_idle() {
        if (restoring_settings_) {
            return;
        }
        save_persisted_settings();
    }

    void remember_session_tab(const QString& tab) {
        if (tab != QStringLiteral("client") && tab != QStringLiteral("host")) {
            return;
        }
        QSettings settings("ArchStreamer", "ArchStreamer");
        settings.setValue("ui/lastSessionTab", tab);
        persist_settings_if_idle();
    }

    void restore_last_session_tab() {
        if (tabs_ == nullptr) {
            return;
        }
        QSettings settings("ArchStreamer", "ArchStreamer");
        const auto tab = settings.value("ui/lastSessionTab", "client").toString().trimmed().toLower();
        const QString want = (tab == QStringLiteral("host"))
            ? QStringLiteral("Host")
            : QStringLiteral("Client");
        for (int i = 0; i < tabs_->count(); ++i) {
            if (tabs_->tabText(i) == want) {
                tabs_->setCurrentIndex(i);
                return;
            }
        }
    }

    void apply_log_level_from_settings() {
        gui_log_level.store(static_cast<int>(current_log_level()));
    }

    GuiLogLevel current_log_level() const {
        if (settings_log_level_ == nullptr) {
            return GuiLogLevel::Normal;
        }
        return static_cast<GuiLogLevel>(settings_log_level_->currentData().toInt());
    }

    int session_timeout_seconds() const {
        if (settings_session_timeout_ == nullptr) {
            return 30;
        }
        return settings_session_timeout_->value();
    }

    std::filesystem::path art_root_path() const {
        if (settings_art_root_ != nullptr && !settings_art_root_->text().trimmed().isEmpty()) {
            return std::filesystem::path{settings_art_root_->text().trimmed().toStdString()};
        }
        return std::filesystem::path{archstreamer::DefaultArtRoot};
    }

    std::string steam_account_id_text() const {
        if (profile_steam_account_ == nullptr) {
            return {};
        }
        return profile_steam_account_->text().trimmed().toStdString();
    }

    std::string profile_client_username() const {
        if (profile_username_ == nullptr) {
            return "local";
        }
        const auto text = profile_username_->text().trimmed();
        return text.isEmpty() ? std::string("local") : text.toStdString();
    }

    std::string profile_host_name() const {
        if (profile_host_name_ == nullptr) {
            return profile_client_username();
        }
        const auto text = profile_host_name_->text().trimmed();
        return text.isEmpty() ? profile_client_username() : text.toStdString();
    }

    void apply_art_root_to_pickers() {
        const auto art_root = art_root_path();
#ifdef ARCHSTREAMER_HAS_HOST
        if (host_game_picker_ != nullptr) {
            host_game_picker_->setArtRoot(art_root);
        }
#endif
        // Don't overwrite client host-art cache after a successful Connect.
        if (client_game_picker_ != nullptr && !client_catalog_loaded_) {
            client_game_picker_->setArtRoot(art_root);
        }
    }

    void detect_steam_account() {
        const auto account = archstreamer::resolve_steam_account();
        if (!account.has_value()) {
            append_log(profile_log_, "No Steam userdata account found (checked common Steam install paths).");
            return;
        }
        const auto text = QString::fromStdString(account->account_id);
        if (profile_steam_account_ != nullptr) {
            profile_steam_account_->setText(text);
        }
        save_persisted_settings();
        append_log(
            profile_log_,
            QString("Detected Steam account %1 (%2)")
                .arg(text, QString::fromStdString(account->config_dir.string())));
    }

    void refresh_art_from_steam() {
#ifndef ARCHSTREAMER_HAS_HOST
        append_log(
            settings_log_,
            "Steam art refresh from a local ROM catalog requires a host-capable build.");
#else
        if (art_refresh_thread_.joinable()) {
            if (art_refreshing_.load()) {
                append_log(settings_log_, "Art refresh already running.");
                return;
            }
            art_refresh_thread_.join();
        }

        const auto rom_root = host_rom_root_ != nullptr
            ? std::filesystem::path{host_rom_root_->text().toStdString()}
            : art_root_path().parent_path() / "Games";
        const auto meta_root = host_meta_root_ != nullptr
            ? std::filesystem::path{host_meta_root_->text().toStdString()}
            : art_root_path().parent_path() / "Meta";
        const auto art_root = art_root_path();
        const auto steam_account_id = steam_account_id_text();

        append_log(
            settings_log_,
            steam_account_id.empty()
                ? "Refreshing art from Steam grid (auto-detect account)..."
                : QString("Refreshing art from Steam account %1...")
                    .arg(QString::fromStdString(steam_account_id)));
        art_refreshing_ = true;
        art_refresh_thread_ = std::thread([this, rom_root, meta_root, art_root, steam_account_id] {
            QString message;
            try {
                const auto catalog = archstreamer::scan_game_catalog(
                    rom_root,
                    archstreamer::LibretroCoreRegistry::ubuntu_defaults(),
                    meta_root);
                const auto list = catalog.list();
                std::vector<archstreamer::GameArtImportTarget> targets;
                targets.reserve(list.games.size());
                for (const auto& game : list.games) {
                    archstreamer::GameArtImportTarget target;
                    target.asset_key = game.asset_key;
                    target.display_name = game.display_name;
                    target.canonical_name = game.canonical_name;
                    if (const auto hosted = catalog.find_hosted(game.id); hosted.has_value()) {
                        target.content_path = hosted->get().content_path;
                    }
                    targets.push_back(std::move(target));
                }

                archstreamer::SteamArtImportOptions options;
                options.steam_account_id = steam_account_id;
                options.replace_when_different = true;
                const auto result = archstreamer::import_steam_grid_art(targets, art_root, options);
                message = QString(
                    "Art refresh done: account=%1 shortcuts=%2 matched=%3 copied=%4 replaced=%5 skipped=%6 unmatched=%7")
                    .arg(QString::fromStdString(result.resolved_account_id))
                    .arg(result.shortcuts_read)
                    .arg(result.matched_games)
                    .arg(result.files_copied)
                    .arg(result.files_replaced)
                    .arg(result.files_skipped)
                    .arg(result.unmatched_shortcuts.size());
                if (result.shortcuts_read == 0) {
                    message += " (no Steam shortcuts found)";
                }
            } catch (const std::exception& error) {
                message = QString("Art refresh failed: %1").arg(error.what());
            }

            QMetaObject::invokeMethod(
                this,
                [this, message = std::move(message)] {
                    art_refreshing_ = false;
                    append_log(settings_log_, message);
                    QPixmapCache::clear();
                    apply_art_root_to_pickers();
                    if (host_game_picker_ != nullptr) {
                        host_game_picker_->refreshArtDisplay();
                    }
                    if (client_game_picker_ != nullptr) {
                        client_game_picker_->refreshArtDisplay();
                    }
                },
                Qt::QueuedConnection);
        });
#endif
    }

    archstreamer::GameFilter client_filter_from_fields() const {
        archstreamer::GameFilter filter;
        filter.requested_players = static_cast<std::uint8_t>(client_players_->value());
        if (selected_mode(client_mode_) == archstreamer::GameSessionMode::Multiplayer) {
            filter.mode = archstreamer::GameFilterMode::Multiplayer;
        } else {
            filter.mode = archstreamer::GameFilterMode::SinglePlayer;
        }
        return filter;
    }

    void refresh_filtered_client_games() {
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

    archstreamer::ClientAppConfig client_config_from_fields() const {
        archstreamer::ClientAppConfig config;
        config.host = client_host_->text().toStdString();
        config.control_port = static_cast<std::uint16_t>(client_port_->value());
        config.input_port = static_cast<std::uint16_t>(client_input_port_->value());
        config.username = profile_client_username();
        config.display_name = config.username;
        config.role = selected_client_role(client_role_);
        config.session_mode = selected_mode(client_mode_);
        config.filter = client_filter_from_fields();
        config.wants_video = client_video_->isChecked();
        config.wants_audio = client_audio_->isChecked();
        config.send_keyboard = client_send_keyboard_ != nullptr && client_send_keyboard_->isChecked();
        config.synced_av = client_synced_av_ != nullptr && client_synced_av_->isChecked();
        config.wanted_tier = selected_stream_quality();
        config.show_framecount =
            settings_show_framecount_ != nullptr && settings_show_framecount_->isChecked();

        for (const auto* item : client_controllers_->selectedItems()) {
            config.controller_indexes.push_back(static_cast<std::size_t>(client_controllers_->row(item)));
        }
        return config;
    }

    archstreamer::MediaQualityTier selected_stream_quality() const {
        if (client_stream_quality_ == nullptr || client_stream_quality_->currentData().isNull()) {
            return archstreamer::MediaQualityTier::Auto;
        }
        return static_cast<archstreamer::MediaQualityTier>(client_stream_quality_->currentData().toInt());
    }

    void apply_client_host(const QString& address, int control_port, int input_port, const QString& label = {}) {
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

    void update_client_host_summary(const QString& label = {}) {
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

    void open_host_search_dialog() {
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

    void start_client_host_auto_pick() {
        if (client_host_ == nullptr || !client_host_->text().trimmed().isEmpty()) {
            return;
        }
        try {
            client_auto_browser_ = std::make_unique<archstreamer::HostDiscoveryBrowser>();
        } catch (const std::exception& error) {
            append_log(client_log_, QString("Host auto-pick unavailable: %1").arg(error.what()));
            return;
        }
        client_auto_pick_timer_ = new QTimer(this);
        client_auto_pick_timer_->setInterval(1000);
        client_auto_pick_attempts_ = 0;
        connect(client_auto_pick_timer_, &QTimer::timeout, this, [this] {
            if (client_host_ == nullptr || !client_host_->text().trimmed().isEmpty() || !client_auto_browser_) {
                stop_client_host_auto_pick();
                return;
            }
            try {
                client_auto_browser_->poll();
                client_auto_browser_->expire_older_than(std::chrono::seconds(8));
                if (const auto preferred = archstreamer::prefer_discovered_host(client_auto_browser_->hosts());
                    preferred.has_value()) {
                    apply_client_host(
                        QString::fromStdString(preferred->address),
                        preferred->control_port,
                        preferred->input_port,
                        QString::fromStdString(preferred->username));
                    append_log(client_log_, "Auto-selected LAN host (same-subnet preferred).");
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

    void stop_client_host_auto_pick() {
        if (client_auto_pick_timer_ != nullptr) {
            client_auto_pick_timer_->stop();
            client_auto_pick_timer_->deleteLater();
            client_auto_pick_timer_ = nullptr;
        }
        client_auto_browser_.reset();
    }

    void connect_client() {
        if (client_host_ == nullptr || client_host_->text().trimmed().isEmpty()) {
            append_log(client_log_, "Select a host (Select Host… or This PC) before Connect.");
            return;
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

    void start_client() {
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
        disc_control_ = std::make_shared<archstreamer::DiscControlBridge>();
        heartbeat_prefs_ = std::make_shared<archstreamer::ClientHeartbeatPrefs>();
        {
            std::lock_guard lock(heartbeat_prefs_->mutex);
            heartbeat_prefs_->wanted_tier = config.wanted_tier;
            heartbeat_prefs_->max_bitrate_kbps = config.max_bitrate_kbps;
            heartbeat_prefs_->show_framecount = config.show_framecount;
        }
        client_thread_ = std::thread([this, config = std::move(config)]() mutable {
            try {
                auto connected_client_id = std::optional<archstreamer::ClientId>{};
                archstreamer::ClientAppCallbacks callbacks;
                callbacks.disc_control = disc_control_;
                callbacks.heartbeat_prefs = heartbeat_prefs_;
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
                        append_log(client_log_, "Starting GStreamer video receiver (separate window).");
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
            QMetaObject::invokeMethod(
                client_catalog_status_,
                [this] {
                    client_catalog_status_->setText("Client stopped");
                    refresh_game_options_ui();
                },
                Qt::QueuedConnection);
            append_log(client_log_, "Client worker stopped.", GuiLogLevel::Quiet);
        });
    }

    void stop_client() {
        client_stop_requested_ = true;
        if (client_thread_.joinable()) {
            client_thread_.join();
        }
        if (disc_control_) {
            std::lock_guard lock(disc_control_->mutex);
            disc_control_->session_active = false;
        }
        heartbeat_prefs_.reset();
    }

    void stop_client_connect() {
        if (client_connect_thread_.joinable()) {
            client_connect_thread_.join();
        }
    }

#ifdef ARCHSTREAMER_HAS_HOST
    void start_host() {
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
        if (!host_game_picker_->hasSelection()) {
            append_log(host_log_, "Choose a host game before starting.");
            return;
        }

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
            host_cfg.selector = *host_game_picker_->selectedGameId();
            const auto selected_id = *host_cfg.selector;
            persisted_host_game_id_ = QString::fromStdString(selected_id);
            // Keep Client Join in sync for local host+client testing.
            persisted_client_game_id_ = persisted_host_game_id_;
            if (client_game_picker_ != nullptr) {
                client_game_picker_->setSelectedGameId(selected_id);
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

    void stop_host() {
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

    void stop_host_local_media() {
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

    void sync_host_local_media() {
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

    archstreamer::ClientApp client_app_;
    archstreamer::GameList client_full_catalog_;
    bool client_catalog_loaded_ = false;
    bool restoring_settings_ = false;
    QTabWidget* tabs_ = nullptr;
    QString client_host_label_;
    QString persisted_client_game_id_;
#ifdef ARCHSTREAMER_HAS_HOST
    QString persisted_host_game_id_;
    bool syncing_host_role_ = false;
#endif
    std::atomic_bool client_stop_requested_ = false;
    std::atomic_bool client_connecting_ = false;
    std::atomic_bool art_refreshing_ = false;
    std::thread client_connect_thread_;
    std::thread client_thread_;
    std::thread art_refresh_thread_;
#ifdef ARCHSTREAMER_HAS_HOST
    QProcess* host_process_ = nullptr;
    QStringList host_debug_args_;
    std::unique_ptr<archstreamer::HostDiscoveryAnnouncer> host_announcer_;
    std::unique_ptr<archstreamer::ClientMediaPlayback> host_local_receiver_;
    QTimer* host_local_media_poll_timer_ = nullptr;
    QTimer* host_advertise_timer_ = nullptr;
#endif

    QLineEdit* client_host_ = nullptr;
    QLabel* client_host_summary_ = nullptr;
    QSpinBox* client_port_ = nullptr;
    QSpinBox* client_input_port_ = nullptr;
    QComboBox* client_role_ = nullptr;
    QComboBox* client_mode_ = nullptr;
    QSpinBox* client_players_ = nullptr;
    QCheckBox* client_video_ = nullptr;
    QCheckBox* client_audio_ = nullptr;
    QCheckBox* client_send_keyboard_ = nullptr;
    QComboBox* client_stream_quality_ = nullptr;
    QCheckBox* client_synced_av_ = nullptr;
    QLabel* client_catalog_status_ = nullptr;
    archstreamer::gui::GamePickerWidget* client_game_picker_ = nullptr;
    QListWidget* client_controllers_ = nullptr;
    QPlainTextEdit* client_log_ = nullptr;
    std::shared_ptr<archstreamer::DiscControlBridge> disc_control_;
    std::shared_ptr<archstreamer::ClientHeartbeatPrefs> heartbeat_prefs_;
    QLabel* game_options_status_ = nullptr;
    QComboBox* game_options_disc_ = nullptr;
    QPushButton* game_options_swap_ = nullptr;
    QPushButton* game_options_prev_ = nullptr;
    QPushButton* game_options_next_ = nullptr;
    QTimer* game_options_poll_timer_ = nullptr;
    std::unique_ptr<archstreamer::HostDiscoveryBrowser> client_auto_browser_;
    QTimer* client_auto_pick_timer_ = nullptr;
    int client_auto_pick_attempts_ = 0;

#ifdef ARCHSTREAMER_HAS_HOST
    QLineEdit* host_rom_root_ = nullptr;
    QLineEdit* host_meta_root_ = nullptr;
    QSpinBox* host_control_port_ = nullptr;
    QSpinBox* host_input_port_ = nullptr;
    QSpinBox* host_video_port_ = nullptr;
    QSpinBox* host_audio_port_ = nullptr;
    QSpinBox* host_clients_ = nullptr;
    QComboBox* host_role_ = nullptr;
    QComboBox* host_mode_ = nullptr;
    QComboBox* host_bridge_controller_ = nullptr;
    QCheckBox* host_video_ = nullptr;
    QCheckBox* host_audio_ = nullptr;
    QCheckBox* host_local_media_ = nullptr;
    QCheckBox* host_advertise_ = nullptr;
    QLabel* host_status_ = nullptr;
    archstreamer::gui::GamePickerWidget* host_game_picker_ = nullptr;
    QPlainTextEdit* host_log_ = nullptr;
#endif

    QLineEdit* profile_username_ = nullptr;
    QLineEdit* profile_host_name_ = nullptr;
    QLineEdit* profile_steam_account_ = nullptr;
    QPlainTextEdit* profile_log_ = nullptr;

    QLineEdit* settings_art_root_ = nullptr;
    QSpinBox* settings_session_timeout_ = nullptr;
    QComboBox* settings_log_level_ = nullptr;
    QCheckBox* settings_show_framecount_ = nullptr;
#ifdef ARCHSTREAMER_HAS_HOST
    QLineEdit* settings_native_host_runner_ = nullptr;
#endif
#ifdef ARCHSTREAMER_HAS_HOST
    QComboBox* settings_gpu_ = nullptr;
    QCheckBox* settings_separate_render_gpu_ = nullptr;
    QComboBox* settings_render_gpu_ = nullptr;
    QComboBox* settings_renderer_ = nullptr;
    QComboBox* settings_yuzu_scale_ = nullptr;
    QComboBox* settings_retroarch_scale_ = nullptr;
#endif
    QComboBox* settings_audio_out_ = nullptr;
    QPlainTextEdit* settings_log_ = nullptr;
};

} // namespace

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    write_to_log_file("[" + log_timestamp().toStdString() + "] === archstreamer_gui started ===");
    write_to_log_file("[" + log_timestamp().toStdString() + "] Log file: " + gui_log_path().string());

    QApplication app(argc, argv);
    RemotedKeyboardEventFilter keyboard_filter;
    app.installEventFilter(&keyboard_filter);
    MainWindow window;
    window.show();

    for (int index = 1; index + 1 < argc; ++index) {
        if (QString::fromLocal8Bit(argv[index]) == "--debug-profile") {
            mirror_gui_logs_to_stdout = true;
            const auto profile = QString::fromLocal8Bit(argv[index + 1]);
            QTimer::singleShot(0, &window, [&window, profile] {
                window.apply_debug_profile(profile);
            });
            break;
        }
    }

    return app.exec();
}
