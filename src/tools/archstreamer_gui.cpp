#include "common/catalog_paths.hpp"
#include "common/catalog_presenter.hpp"
#include "client/client_app.hpp"
#include "client/game_filter.hpp"
#include "game_picker_widget.hpp"
#include "host_picker_widget.hpp"
#include "common/discovery.hpp"
#include "common/game_assets.hpp"
#include "common/platform/paths.hpp"
#include "common/steam_art_import.hpp"
#ifdef ARCHSTREAMER_HAS_HOST
#include "host/game_catalog_scanner.hpp"
#endif

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPixmapCache>
#include <QProcess>
#include <QCoreApplication>
#include <QPushButton>
#include <QSettings>
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

std::atomic_bool mirror_gui_logs_to_stdout = false;

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

void append_log(QPlainTextEdit* log, QString message) {
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
#endif

class MainWindow final : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle("ArchStreamer");
        resize(1100, 720);

        auto* tabs = new QTabWidget(this);
        tabs->addTab(build_client_tab(), "Client");
#ifdef ARCHSTREAMER_HAS_HOST
        tabs->addTab(build_host_tab(), "Host");
#endif
        tabs->addTab(build_settings_tab(), "Settings");
        setCentralWidget(tabs);
        load_persisted_settings();
    }

    ~MainWindow() override {
        save_persisted_settings();
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

        client_host_ = new QLineEdit(form_box);
        client_host_->setPlaceholderText("select a LAN host below, or type an IP");
        client_host_->setClearButtonEnabled(true);
        client_port_ = new QSpinBox(form_box);
        client_port_->setRange(1, 65535);
        client_port_->setValue(45555);
        client_input_port_ = new QSpinBox(form_box);
        client_input_port_->setRange(1, 65535);
        client_input_port_->setValue(DefaultInputPort);
        client_username_ = new QLineEdit(form_box);
        {
            const auto username = QString::fromStdString(archstreamer::current_username());
            client_username_->setText(username.isEmpty() ? QStringLiteral("local") : username);
        }
        client_role_ = new QComboBox(form_box);
        client_role_->addItems({"Player", "Viewer"});
        client_mode_ = new QComboBox(form_box);
        client_mode_->addItems({"Singleplayer", "Multiplayer"});
        client_players_ = new QSpinBox(form_box);
        client_players_->setRange(0, 2);
        client_players_->setValue(1);
        client_system_ = new QLineEdit(form_box);
        client_language_ = new QLineEdit(form_box);
        client_video_ = new QCheckBox("Receive video", form_box);
        client_video_->setChecked(true);
        client_audio_ = new QCheckBox("Receive audio", form_box);
        client_audio_->setChecked(true);

        form->addRow("Host", client_host_);
        form->addRow("Control port", client_port_);
        form->addRow("Input port", client_input_port_);
        form->addRow("Username", client_username_);
        form->addRow("Role", client_role_);
        form->addRow("Mode", client_mode_);
        form->addRow("Players", client_players_);
        form->addRow("System filter", client_system_);
        form->addRow("Language filter", client_language_);
        form->addRow("", client_video_);
        form->addRow("", client_audio_);

        client_host_picker_ = new archstreamer::gui::HostPickerWidget(page);
        connect(client_host_picker_, &archstreamer::gui::HostPickerWidget::hostSelected, this,
            [this](const QString& address, int control_port, int input_port) {
                const auto changed =
                    client_host_->text() != address ||
                    client_port_->value() != control_port ||
                    client_input_port_->value() != input_port;
                client_host_->setText(address);
                client_port_->setValue(control_port);
                client_input_port_->setValue(input_port);
                if (changed) {
                    append_log(client_log_, QString("Selected host %1 (control %2, input %3)")
                        .arg(address)
                        .arg(control_port)
                        .arg(input_port));
                }
            });

        client_game_picker_ = new archstreamer::gui::GamePickerWidget(page);
        client_game_picker_->setArtRoot(art_root_path());
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
        connect(connect_host, &QPushButton::clicked, this, [this] {
            connect_client();
        });
        connect(join, &QPushButton::clicked, this, [this] {
            start_client();
        });
        connect(stop, &QPushButton::clicked, this, [this] {
            stop_client();
        });
        connect(client_role_, &QComboBox::currentIndexChanged, this, [this] {
            if (selected_client_role(client_role_) == archstreamer::ClientParticipantRole::Viewer) {
                client_players_->setValue(0);
                client_controllers_->clearSelection();
            } else if (client_players_->value() == 0) {
                client_players_->setValue(1);
            }
            refresh_filtered_client_games();
        });
        connect(client_mode_, &QComboBox::currentIndexChanged, this, [this] {
            refresh_filtered_client_games();
        });
        connect(client_players_, qOverload<int>(&QSpinBox::valueChanged), this, [this] {
            refresh_filtered_client_games();
        });
        connect(client_system_, &QLineEdit::textChanged, this, [this] {
            refresh_filtered_client_games();
        });
        connect(client_language_, &QLineEdit::textChanged, this, [this] {
            refresh_filtered_client_games();
        });

        auto* left = new QVBoxLayout();
        left->addWidget(form_box);
        left->addWidget(client_host_picker_);
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

#ifdef ARCHSTREAMER_HAS_HOST
    QWidget* build_host_tab() {
        auto* page = new QWidget(this);
        auto* root = new QHBoxLayout(page);

        auto* form_box = new QGroupBox("Host Runner", page);
        auto* form = new QFormLayout(form_box);

        host_rom_root_ = new QLineEdit(archstreamer::DefaultRomRoot, form_box);
        host_meta_root_ = new QLineEdit(archstreamer::DefaultMetaRoot, form_box);
        host_username_ = new QLineEdit(form_box);
        {
            const auto username = QString::fromStdString(archstreamer::current_username());
            host_username_->setText(username.isEmpty() ? QStringLiteral("host") : username);
        }
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
        // Viewer first: LAN remote play is the default host path.
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
        host_advertise_ = new QCheckBox("Advertise on LAN", form_box);
        host_advertise_->setChecked(true);

        form->addRow("ROM root", host_rom_root_);
        form->addRow("Meta root", host_meta_root_);
        form->addRow("Display name", host_username_);
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
        form->addRow("", host_advertise_);

        auto* start = new QPushButton("Start Host", page);
        auto* stop = new QPushButton("Stop Host", page);
        auto* load_games = new QPushButton("Load Games", page);
        auto* refresh_host_controllers_button = new QPushButton("Refresh Controllers", page);
        host_status_ = new QLabel("Host stopped", page);
        host_game_picker_ = new archstreamer::gui::GamePickerWidget(page);
        host_game_picker_->setArtRoot(art_root_path());
        connect(start, &QPushButton::clicked, this, [this] {
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
        connect(host_role_, &QComboBox::currentIndexChanged, this, [this] {
            sync_host_role_and_bridge();
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
        });
        connect(host_control_port_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
            client_port_->setValue(value);
        });
        connect(host_input_port_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
            client_input_port_->setValue(value);
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
        load_host_games();
        return page;
    }
#endif // ARCHSTREAMER_HAS_HOST

    QWidget* build_settings_tab() {
        auto* page = new QWidget(this);
        auto* root = new QHBoxLayout(page);

        auto* form_box = new QGroupBox("Local configuration", page);
        auto* form = new QFormLayout(form_box);

        settings_art_root_ = new QLineEdit(archstreamer::DefaultArtRoot, form_box);
        settings_steam_account_ = new QLineEdit(form_box);
        settings_steam_account_->setPlaceholderText("auto-detect if empty");
        auto* steam_row = new QWidget(form_box);
        auto* steam_layout = new QHBoxLayout(steam_row);
        steam_layout->setContentsMargins(0, 0, 0, 0);
        auto* detect_steam = new QPushButton("Detect", steam_row);
        steam_layout->addWidget(settings_steam_account_, 1);
        steam_layout->addWidget(detect_steam);

        settings_session_timeout_ = new QSpinBox(form_box);
        settings_session_timeout_->setRange(5, 3600);
        settings_session_timeout_->setValue(30);
        settings_session_timeout_->setSuffix(" s");
        settings_session_timeout_->setToolTip(
            "How long the host waits for remote clients to join before giving up.\n"
            "Increase this when testing LAN connections between machines.");

        form->addRow("Art root (host / local import)", settings_art_root_);
        form->addRow("Steam account ID", steam_row);
        form->addRow("Host lobby wait", settings_session_timeout_);

        auto* refresh_art = new QPushButton("Refresh Art from Steam", form_box);
        form->addRow("", refresh_art);

        connect(settings_art_root_, &QLineEdit::editingFinished, this, [this] {
            apply_art_root_to_pickers();
            save_persisted_settings();
        });
        connect(settings_art_root_, &QLineEdit::textChanged, this, [this](const QString&) {
            apply_art_root_to_pickers();
        });
        connect(settings_steam_account_, &QLineEdit::editingFinished, this, [this] {
            save_persisted_settings();
        });
        connect(settings_session_timeout_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
            save_persisted_settings();
        });
        connect(detect_steam, &QPushButton::clicked, this, [this] {
            detect_steam_account();
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
            "Steam account ID: leave blank to auto-detect.\n"
            "Host lobby wait is how long the host keeps accepting remote joins.",
#else
            "Art root is used for local Steam import when available.\n"
            "Clients cache host art under the ArchStreamer cache directory.\n"
            "Steam account ID: leave blank to auto-detect.\n"
            "Host lobby wait is unused in this client-only build.",
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
            append_log(client_log_, QString("Controller scan failed: %1").arg(error.what()));
        }
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
            append_log(host_log_, QString("Host controller scan failed: %1").arg(error.what()));
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
        syncing_host_role_ = false;
    }

    void sync_host_advertise(bool enabled) {
        if (enabled) {
            if (!host_announcer_) {
                try {
                    host_announcer_ = std::make_unique<archstreamer::HostDiscoveryAnnouncer>(
                        archstreamer::HostAnnouncement{
                            host_username_->text().toStdString(),
                            static_cast<std::uint16_t>(host_control_port_->value()),
                            static_cast<std::uint16_t>(host_input_port_->value()),
                        });
                } catch (const std::exception& error) {
                    append_log(host_log_, QString("Advertise failed: %1").arg(error.what()));
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
        if (!host_announcer_ || host_username_ == nullptr) {
            return;
        }
        try {
            host_announcer_->set_announcement(archstreamer::HostAnnouncement{
                host_username_->text().toStdString(),
                static_cast<std::uint16_t>(host_control_port_->value()),
                static_cast<std::uint16_t>(host_input_port_->value()),
            });
            host_announcer_->advertise();
        } catch (const std::exception& error) {
            append_log(host_log_, QString("Advertise error: %1 (check firewall UDP 45550)").arg(error.what()));
        }
    }

    void load_host_games() {
        try {
            const auto catalog = archstreamer::scan_game_catalog(
                host_rom_root_->text().toStdString(),
                archstreamer::LibretroCoreRegistry::ubuntu_defaults(),
                host_meta_root_->text().toStdString());
            const auto list = catalog.list();
            host_game_picker_->setCatalog(list);
            if (!list.games.empty() && !host_game_picker_->hasSelection()) {
                host_game_picker_->setSelectedGameId(list.games.front().id);
            }
            host_status_->setText(QString("Host stopped; %1 game(s) loaded").arg(list.games.size()));
            append_log(host_log_, QString("Loaded %1 host game(s).").arg(list.games.size()));
        } catch (const std::exception& error) {
            host_game_picker_->setCatalog({});
            host_status_->setText("Host stopped; game load failed");
            append_log(host_log_, QString("Load games failed: %1").arg(error.what()));
        }
    }
#endif // ARCHSTREAMER_HAS_HOST

    void load_persisted_settings() {
        QSettings settings("ArchStreamer", "ArchStreamer");
        const auto art_root = settings.value("paths/artRoot", archstreamer::DefaultArtRoot).toString();
        const auto account = settings.value("steam/accountId").toString().trimmed();
        const auto session_timeout = settings.value("host/sessionTimeoutSeconds", 30).toInt();
        if (settings_art_root_ != nullptr) {
            settings_art_root_->setText(art_root);
        }
        if (settings_steam_account_ != nullptr) {
            settings_steam_account_->setText(account);
        }
        if (settings_session_timeout_ != nullptr) {
            settings_session_timeout_->setValue(qBound(session_timeout, 5, 3600));
        }
        apply_art_root_to_pickers();
        if (!account.isEmpty() && settings_log_ != nullptr) {
            append_log(settings_log_, QString("Loaded Steam account ID %1").arg(account));
        }
    }

    void save_persisted_settings() {
        QSettings settings("ArchStreamer", "ArchStreamer");
        settings.setValue("paths/artRoot", QString::fromStdString(art_root_path().string()));
        settings.setValue("steam/accountId", QString::fromStdString(steam_account_id_text()));
        settings.setValue("host/sessionTimeoutSeconds", session_timeout_seconds());
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
        if (settings_steam_account_ == nullptr) {
            return {};
        }
        return settings_steam_account_->text().trimmed().toStdString();
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
            append_log(settings_log_, "No Steam userdata account found (checked common Steam install paths).");
            return;
        }
        const auto text = QString::fromStdString(account->account_id);
        if (settings_steam_account_ != nullptr) {
            settings_steam_account_->setText(text);
        }
        save_persisted_settings();
        append_log(
            settings_log_,
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
        if (!client_system_->text().isEmpty()) {
            filter.system_name = client_system_->text().toStdString();
        }
        if (!client_language_->text().isEmpty()) {
            filter.language = client_language_->text().toStdString();
        }
        return filter;
    }

    void refresh_filtered_client_games() {
        if (!client_catalog_loaded_) {
            return;
        }
        const auto filter = client_filter_from_fields();
        const auto filtered = archstreamer::filter_games(client_full_catalog_, filter);
        client_game_picker_->setCatalog(filtered);
        client_catalog_status_->setText(QString("%1 game(s) from host, %2 shown")
            .arg(client_full_catalog_.games.size())
            .arg(filtered.games.size()));
    }

    archstreamer::ClientAppConfig client_config_from_fields() const {
        archstreamer::ClientAppConfig config;
        config.host = client_host_->text().toStdString();
        config.control_port = static_cast<std::uint16_t>(client_port_->value());
        config.input_port = static_cast<std::uint16_t>(client_input_port_->value());
        config.username = client_username_->text().toStdString();
        config.display_name = config.username;
        config.role = selected_client_role(client_role_);
        config.session_mode = selected_mode(client_mode_);
        config.filter = client_filter_from_fields();
        config.wants_video = client_video_->isChecked();
        config.wants_audio = client_audio_->isChecked();

        for (const auto* item : client_controllers_->selectedItems()) {
            config.controller_indexes.push_back(static_cast<std::size_t>(client_controllers_->row(item)));
        }
        return config;
    }

    void connect_client() {
        if (client_host_ == nullptr || client_host_->text().trimmed().isEmpty()) {
            append_log(client_log_, "Set Host to a LAN IP (click a discovered host, or type one) before Connect.");
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
                    [this, full = std::move(catalog.full_catalog), filtered = std::move(catalog.filtered_catalog),
                     art_cache = std::move(catalog.art_cache_root)]() mutable {
                        client_full_catalog_ = std::move(full);
                        client_catalog_loaded_ = true;
                        if (!art_cache.empty()) {
                            client_game_picker_->setArtRoot(art_cache);
                            append_log(client_log_, QString("Using host art cache: %1")
                                .arg(QString::fromStdString(art_cache.string())));
                        }
                        client_game_picker_->setCatalog(filtered);
                        client_catalog_status_->setText(QString("%1 game(s) from host, %2 shown")
                            .arg(client_full_catalog_.games.size())
                            .arg(filtered.games.size()));
                        append_log(client_log_, QString("Connected: received %1 games; showing %2 after filters.")
                            .arg(client_full_catalog_.games.size())
                            .arg(filtered.games.size()));
                        if (!filtered.games.empty()) {
                            append_log(
                                client_log_,
                                QString("First game: %1")
                                    .arg(QString::fromStdString(archstreamer::format_game_summary(filtered.games.front()))));
                        } else {
                            append_log(client_log_, "Catalog connected, but no games matched the current filters.");
                        }
                        append_log(
                            client_log_,
                            "Tip: match the host Mode and selected game before Join, or the lobby will reject the hello.");
                    },
                    Qt::QueuedConnection);
            } catch (const std::exception& error) {
                const auto message = QString::fromLocal8Bit(error.what());
                QMetaObject::invokeMethod(
                    this,
                    [this, message] {
                        append_log(client_log_, QString("Connect failed: %1").arg(message));
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
            if (config.filter.requested_players == 0) {
                append_log(client_log_, "Player role requires at least one local player seat.");
                return;
            }
            if (config.controller_indexes.size() < config.filter.requested_players) {
                append_log(
                    client_log_,
                    QString("Select %1 controller(s) before joining as a player.")
                        .arg(config.filter.requested_players));
                return;
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
        if (client_host_picker_ != nullptr) {
            client_host_picker_->setBrowsing(false);
        }
        client_thread_ = std::thread([this, config = std::move(config)]() mutable {
            try {
                auto connected_client_id = std::optional<archstreamer::ClientId>{};
                archstreamer::ClientAppCallbacks callbacks;
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
                            "(Host Player skips streaming — use Host Viewer to stream to remotes).");
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
                    append_log(client_log_, QString("Session ended: %1").arg(QString::fromStdString(reason)));
                };
                callbacks.on_host_disconnected = [this] {
                    append_log(client_log_, "Host disconnected.");
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
                append_log(client_log_, QString("Client error: %1").arg(message));
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
                    if (client_host_picker_ != nullptr) {
                        client_host_picker_->setBrowsing(true);
                    }
                },
                Qt::QueuedConnection);
            append_log(client_log_, "Client worker stopped.");
        });
    }

    void stop_client() {
        client_stop_requested_ = true;
        if (client_thread_.joinable()) {
            client_thread_.join();
        }
        if (client_host_picker_ != nullptr) {
            client_host_picker_->setBrowsing(true);
        }
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
                        append_log(host_log_, line);
                    }
                }
            });
            connect(host_process_, &QProcess::readyReadStandardError, this, [this] {
                const auto text = QString::fromLocal8Bit(host_process_->readAllStandardError()).trimmed();
                if (!text.isEmpty()) {
                    for (const auto& line : text.split('\n')) {
                        append_log(host_log_, line);
                    }
                }
            });
            connect(host_process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
                sync_host_advertise(false);
                host_status_->setText("Host stopped");
                append_log(host_log_, QString("Host exited: code=%1 status=%2")
                    .arg(code)
                    .arg(status == QProcess::NormalExit ? "normal" : "crashed"));
            });
        }

        QStringList args;
        args
            << "--rom-root" << host_rom_root_->text()
            << "--meta-root" << host_meta_root_->text()
            << "--art-root" << QString::fromStdString(art_root_path().string())
            << "--control-port" << QString::number(host_control_port_->value())
            << "--input-port" << QString::number(host_input_port_->value())
            << "--clients" << QString::number(host_clients_->value())
            << "--session-timeout" << QString::number(session_timeout_seconds())
            << "--host-role" << host_role_text(host_role_)
            << "--mode" << mode_name(selected_mode(host_mode_));
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
        if (bridge_index >= 0) {
            args << "--bridge-controller" << QString::number(bridge_index);
        }
        if (host_video_->isChecked()) {
            if (!host_role_is_viewer(host_role_)) {
                append_log(
                    host_log_,
                    "Stream video left off for Host Player so RetroArch stays on your screen. "
                    "Use Host Viewer when you need to stream to remotes.");
            } else {
                args << "--video" << "--video-port" << QString::number(host_video_port_->value());
            }
        }
        if (host_audio_->isChecked()) {
            if (!host_role_is_viewer(host_role_)) {
                // Local host audio plays through RetroArch/Pulse normally; skip capture fanout.
                append_log(host_log_, "Stream audio left off for Host Player (local audio via RetroArch).");
            } else {
                args << "--audio" << "--audio-port" << QString::number(host_audio_port_->value());
                append_log(
                    host_log_,
                    QString("Audio streaming enabled on base UDP port %1 (captures default output monitor).")
                        .arg(host_audio_port_->value()));
            }
        }
        args << host_debug_args_;
        if (!host_game_picker_->hasSelection()) {
            append_log(host_log_, "Choose a host game before starting.");
            return;
        }
        args << QString::fromStdString(*host_game_picker_->selectedGameId());

        client_port_->setValue(host_control_port_->value());
        client_input_port_->setValue(host_input_port_->value());
        client_mode_->setCurrentIndex(host_mode_->currentIndex());

        const auto program = host_runner_program();
        host_status_->setText("Host starting");
        host_log_->appendPlainText("Starting " + program + " " + args.join(' '));
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
        host_process_->start(program, args);
        if (!host_process_->waitForStarted(3000)) {
            host_status_->setText("Host failed to start");
            host_log_->appendPlainText("Failed to start host_runner: " + host_process_->errorString());
            return;
        }
        host_status_->setText(QString("Host running on port %1").arg(host_control_port_->value()));
        if (host_advertise_ != nullptr && host_advertise_->isChecked()) {
            sync_host_advertise(true);
        }
    }

    void stop_host() {
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
#endif // ARCHSTREAMER_HAS_HOST

    archstreamer::ClientApp client_app_;
    archstreamer::GameList client_full_catalog_;
    bool client_catalog_loaded_ = false;
#ifdef ARCHSTREAMER_HAS_HOST
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
    QTimer* host_advertise_timer_ = nullptr;
#endif

    QLineEdit* client_host_ = nullptr;
    QSpinBox* client_port_ = nullptr;
    QSpinBox* client_input_port_ = nullptr;
    QLineEdit* client_username_ = nullptr;
    QComboBox* client_role_ = nullptr;
    QComboBox* client_mode_ = nullptr;
    QSpinBox* client_players_ = nullptr;
    QLineEdit* client_system_ = nullptr;
    QLineEdit* client_language_ = nullptr;
    QCheckBox* client_video_ = nullptr;
    QCheckBox* client_audio_ = nullptr;
    QLabel* client_catalog_status_ = nullptr;
    archstreamer::gui::HostPickerWidget* client_host_picker_ = nullptr;
    archstreamer::gui::GamePickerWidget* client_game_picker_ = nullptr;
    QListWidget* client_controllers_ = nullptr;
    QPlainTextEdit* client_log_ = nullptr;

#ifdef ARCHSTREAMER_HAS_HOST
    QLineEdit* host_rom_root_ = nullptr;
    QLineEdit* host_meta_root_ = nullptr;
    QLineEdit* host_username_ = nullptr;
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
    QCheckBox* host_advertise_ = nullptr;
    QLabel* host_status_ = nullptr;
    archstreamer::gui::GamePickerWidget* host_game_picker_ = nullptr;
    QPlainTextEdit* host_log_ = nullptr;
#endif

    QLineEdit* settings_art_root_ = nullptr;
    QLineEdit* settings_steam_account_ = nullptr;
    QSpinBox* settings_session_timeout_ = nullptr;
    QPlainTextEdit* settings_log_ = nullptr;
};

} // namespace

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    write_to_log_file("[" + log_timestamp().toStdString() + "] === archstreamer_gui started ===");
    write_to_log_file("[" + log_timestamp().toStdString() + "] Log file: " + gui_log_path().string());

    QApplication app(argc, argv);
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
