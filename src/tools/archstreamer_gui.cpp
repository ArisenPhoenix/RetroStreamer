#include "common/catalog_paths.hpp"
#include "common/catalog_presenter.hpp"
#include "client/client_app.hpp"
#include "client/game_filter.hpp"
#include "game_picker_widget.hpp"
#include "host/game_catalog_scanner.hpp"

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
#include <QProcess>
#include <QCoreApplication>
#include <QPushButton>
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
    const auto now = std::chrono::system_clock::now();
    const auto time_t = std::chrono::system_clock::to_time_t(now);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%H:%M:%S", std::localtime(&time_t));
    f << '[' << ts << "] " << message << '\n';
    f.flush();
}

QString mode_name(archstreamer::GameSessionMode mode) {
    return mode == archstreamer::GameSessionMode::SinglePlayer ? "singleplayer" : "multiplayer";
}

void append_log(QPlainTextEdit* log, QString message) {
    write_to_log_file(message.toStdString());
    if (mirror_gui_logs_to_stdout.load()) {
        std::cout << message.toStdString() << '\n';
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

QString host_role_text(const QComboBox* combo) {
    return combo->currentIndex() == 1 ? "viewer" : "player";
}

QString host_runner_program() {
    const auto app_dir = std::filesystem::path(QCoreApplication::applicationDirPath().toStdString());
    const auto sibling = app_dir / "host_runner";
    if (std::filesystem::exists(sibling)) {
        return QString::fromStdString(sibling.string());
    }

    const auto repo_relative = std::filesystem::current_path() / "build" / "host_runner";
    if (std::filesystem::exists(repo_relative)) {
        return QString::fromStdString(repo_relative.string());
    }

    return "./build/host_runner";
}

class MainWindow final : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle("ArchStreamer");
        resize(1100, 720);

        auto* tabs = new QTabWidget(this);
        tabs->addTab(build_client_tab(), "Client");
        tabs->addTab(build_host_tab(), "Host");
        setCentralWidget(tabs);
    }

    ~MainWindow() override {
        stop_client();
        stop_client_connect();
        stop_host();
    }

    void apply_debug_profile(const QString& profile) {
        if (profile != "local-viewer") {
            append_log(host_log_, "Unknown debug profile: " + profile);
            return;
        }

        host_video_->setChecked(true);
        host_audio_->setChecked(true);
        host_role_->setCurrentIndex(1);
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
    }

private:
    QWidget* build_client_tab() {
        auto* page = new QWidget(this);
        auto* root = new QHBoxLayout(page);

        auto* form_box = new QGroupBox("Client Session", page);
        auto* form = new QFormLayout(form_box);

        client_host_ = new QLineEdit("127.0.0.1", form_box);
        client_port_ = new QSpinBox(form_box);
        client_port_->setRange(1, 65535);
        client_port_->setValue(45555);
        client_input_port_ = new QSpinBox(form_box);
        client_input_port_->setRange(1, 65535);
        client_input_port_->setValue(DefaultInputPort);
        client_username_ = new QLineEdit(qEnvironmentVariable("USER", "local"), form_box);
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

        client_game_picker_ = new archstreamer::gui::GamePickerWidget(page);
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
        client_log_->setReadOnly(true);
        root->addLayout(left, 1);
        root->addWidget(client_log_, 2);

        refresh_client_controllers();
        return page;
    }

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
        host_role_->addItems({"Player", "Viewer"});
        host_role_->setCurrentIndex(1);
        host_mode_ = new QComboBox(form_box);
        host_mode_->addItems({"Singleplayer", "Multiplayer"});
        host_bridge_controller_ = new QComboBox(form_box);
        host_video_ = new QCheckBox("Stream video", form_box);
        host_audio_ = new QCheckBox("Stream audio", form_box);

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

        auto* start = new QPushButton("Start Host", page);
        auto* stop = new QPushButton("Stop Host", page);
        auto* load_games = new QPushButton("Load Games", page);
        auto* refresh_host_controllers_button = new QPushButton("Refresh Controllers", page);
        host_status_ = new QLabel("Host stopped", page);
        host_game_picker_ = new archstreamer::gui::GamePickerWidget(page);
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
            if (host_bridge_controller_->currentData().toInt() >= 0) {
                if (host_role_->currentIndex() != 0) {
                    syncing_host_role_ = true;
                    host_role_->setCurrentIndex(0);
                    syncing_host_role_ = false;
                }
            }
        });
        connect(host_control_port_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
            client_port_->setValue(value);
        });
        connect(host_input_port_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
            client_input_port_->setValue(value);
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
        host_log_->setReadOnly(true);
        root->addLayout(left, 1);
        root->addWidget(host_log_, 2);
        refresh_host_controllers();
        load_host_games();
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
        if (host_role_->currentIndex() == 1 && host_bridge_controller_->currentData().toInt() >= 0) {
            host_bridge_controller_->setCurrentIndex(0);
            append_log(host_log_, "Host Viewer selected; bridge controller cleared to None.");
        }
        syncing_host_role_ = false;
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

    bool connect_client() {
        if (client_thread_.joinable()) {
            append_log(client_log_, "Stop the running client session before reconnecting.");
            return false;
        }
        if (client_connect_thread_.joinable()) {
            if (client_connecting_.load()) {
                append_log(client_log_, "Client catalog fetch is already running.");
                return false;
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
                    [this, full = std::move(catalog.full_catalog), filtered = std::move(catalog.filtered_catalog)]() mutable {
                        client_full_catalog_ = std::move(full);
                        client_catalog_loaded_ = true;
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
        return true;
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
                    }
                    if (!endpoint.audio_uri.empty()) {
                        append_log(client_log_, QString("Audio: %1").arg(QString::fromStdString(endpoint.audio_uri)));
                        append_log(client_log_, "Starting GStreamer audio receiver (autoaudiosink).");
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
                };
                callbacks.on_waiting_without_input = [this] {
                    append_log(client_log_, "Waiting for session end (no input streaming).");
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
    }

    void stop_client_connect() {
        if (client_connect_thread_.joinable()) {
            client_connect_thread_.join();
        }
    }

    void start_host() {
        if (host_process_ != nullptr && host_process_->state() != QProcess::NotRunning) {
            append_log(host_log_, "Host is already running.");
            return;
        }

        if (host_process_ == nullptr) {
            host_process_ = new QProcess(this);
            connect(host_process_, &QProcess::readyReadStandardOutput, this, [this] {
                host_log_->appendPlainText(QString::fromLocal8Bit(host_process_->readAllStandardOutput()));
            });
            connect(host_process_, &QProcess::readyReadStandardError, this, [this] {
                host_log_->appendPlainText(QString::fromLocal8Bit(host_process_->readAllStandardError()));
            });
            connect(host_process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
                host_status_->setText("Host stopped");
                host_log_->appendPlainText(QString("Host exited: code=%1 status=%2")
                    .arg(code)
                    .arg(status == QProcess::NormalExit ? "normal" : "crashed"));
            });
        }

        QStringList args;
        args
            << "--rom-root" << host_rom_root_->text()
            << "--meta-root" << host_meta_root_->text()
            << "--control-port" << QString::number(host_control_port_->value())
            << "--input-port" << QString::number(host_input_port_->value())
            << "--clients" << QString::number(host_clients_->value())
            << "--host-role" << host_role_text(host_role_)
            << "--mode" << mode_name(selected_mode(host_mode_));
        const auto bridge_index = host_bridge_controller_->currentData().toInt();
        if (host_role_->currentIndex() == 0 && bridge_index < 0) {
            host_status_->setText("Host not started");
            append_log(host_log_, "Host role is Player, but no bridge controller is selected.");
            append_log(host_log_, "Select a controller or change Host role to Viewer.");
            return;
        }
        if (host_role_->currentIndex() == 1 && bridge_index >= 0) {
            host_status_->setText("Host not started");
            append_log(host_log_, "Host role is Viewer, but a bridge controller is selected.");
            append_log(host_log_, "Select None for bridge controller or change Host role to Player.");
            return;
        }
        if (bridge_index >= 0) {
            args << "--bridge-controller" << QString::number(bridge_index);
        }
        if (host_video_->isChecked()) {
            args << "--video" << "--video-port" << QString::number(host_video_port_->value());
        }
        if (host_audio_->isChecked()) {
            args << "--audio" << "--audio-port" << QString::number(host_audio_port_->value());
            append_log(
                host_log_,
                QString("Audio streaming enabled on base UDP port %1 (captures default output monitor).")
                    .arg(host_audio_port_->value()));
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
                QString("Host Player reserved as P1; lobby stays open for clients (mode=%1, max clients=%2).")
                    .arg(mode_name(selected_mode(host_mode_)))
                    .arg(host_clients_->value()));
        } else {
            append_log(
                host_log_,
                QString("Host Viewer: waiting for remote players (mode=%1, max clients=%2).")
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
    }

    void stop_host() {
        if (host_process_ == nullptr || host_process_->state() == QProcess::NotRunning) {
            return;
        }
        host_process_->terminate();
        if (!host_process_->waitForFinished(3000)) {
            host_process_->kill();
            host_process_->waitForFinished(3000);
        }
    }

    archstreamer::ClientApp client_app_;
    archstreamer::GameList client_full_catalog_;
    bool client_catalog_loaded_ = false;
    bool syncing_host_role_ = false;
    std::atomic_bool client_stop_requested_ = false;
    std::atomic_bool client_connecting_ = false;
    std::thread client_connect_thread_;
    std::thread client_thread_;
    QProcess* host_process_ = nullptr;
    QStringList host_debug_args_;

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
    archstreamer::gui::GamePickerWidget* client_game_picker_ = nullptr;
    QListWidget* client_controllers_ = nullptr;
    QPlainTextEdit* client_log_ = nullptr;

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
    QLabel* host_status_ = nullptr;
    archstreamer::gui::GamePickerWidget* host_game_picker_ = nullptr;
    QPlainTextEdit* host_log_ = nullptr;
};

} // namespace

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    write_to_log_file("=== archstreamer_gui started ===");
    write_to_log_file("Log file: " + gui_log_path().string());

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
