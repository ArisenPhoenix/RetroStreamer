#include "main_window.hpp"

#include "gui_logging.hpp"
#include "gui_util.hpp"
#include "game_picker_widget.hpp"
#include "host_search_dialog.hpp"
#include "client_video_controller.hpp"
#include "common/catalog_paths.hpp"
#include "common/catalog_presenter.hpp"
#include "common/addresses.hpp"
#include "common/host_addresses.hpp"
#include "common/client_debug_log.hpp"
#include "common/client_logs.hpp"
#include "common/discovery.hpp"
#include "common/discovery_net.hpp"
#include "common/game_assets.hpp"
#include "common/platform/default_platform.hpp"
#include "common/pairing.hpp"
#include "common/platform/paths.hpp"
#include "common/serialization.hpp"
#include "common/steam_art_import.hpp"
#include "client/client_media_playback.hpp"
#include "client/game_filter.hpp"
#include "client/audio_playback_device.hpp"
#include "client/gstreamer_media_pipeline.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <exception>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPixmapCache>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>
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
#include <memory>
#include <optional>
#include <random>
#include <thread>


namespace archstreamer::gui {
namespace {

QString json_string(const QJsonObject& object, const char* key, const QString& fallback = {}) {
    const auto value = object.value(QString::fromLatin1(key));
    return value.isString() ? value.toString() : fallback;
}

int json_int(const QJsonObject& object, const char* key, int fallback) {
    const auto value = object.value(QString::fromLatin1(key));
    return value.isDouble() ? value.toInt(fallback) : fallback;
}

void set_combo_data(QComboBox* combo, int value) {
    if (combo == nullptr) {
        return;
    }
    const QSignalBlocker blocker(combo);
    const auto index = combo->findData(value);
    combo->setCurrentIndex(index >= 0 ? index : 0);
}

QString preferred_pair_lan_ip() {
    const auto addresses = archstreamer::local_ipv4_addresses();
    auto prefer = [&addresses](const char* prefix) -> QString {
        const auto found = std::find_if(addresses.begin(), addresses.end(), [prefix](const std::string& address) {
            return address.rfind(prefix, 0) == 0;
        });
        return found == addresses.end() ? QString() : QString::fromStdString(*found);
    };
    if (auto ip = prefer("192.168."); !ip.isEmpty()) return ip;
    if (auto ip = prefer("10."); !ip.isEmpty()) return ip;
    if (auto ip = prefer("172."); !ip.isEmpty()) return ip;
    return addresses.empty() ? QString() : QString::fromStdString(addresses.front());
}

QPixmap render_pair_qr(const QString& uri) {
    QProcess qr;
    qr.start(
        QStringLiteral("qrencode"),
        {QStringLiteral("-o"), QStringLiteral("-"), QStringLiteral("-t"), QStringLiteral("PNG"), uri});
    if (!qr.waitForFinished(3000) || qr.exitStatus() != QProcess::NormalExit || qr.exitCode() != 0) {
        return {};
    }
    QPixmap pixmap;
    pixmap.loadFromData(qr.readAllStandardOutput(), "PNG");
    return pixmap;
}

void write_http_response(QTcpSocket* socket, int code, const QByteArray& body) {
    const QByteArray status = code == 200 ? "200 OK" :
        code == 401 ? "401 Unauthorized" :
        code == 405 ? "405 Method Not Allowed" :
        code == 400 ? "400 Bad Request" :
        QByteArray::number(code) + " Error";
    socket->write("HTTP/1.1 " + status + "\r\n");
    socket->write("Content-Type: application/json\r\n");
    socket->write("Content-Length: " + QByteArray::number(body.size()) + "\r\n");
    socket->write("Connection: close\r\n\r\n");
    socket->write(body);
    socket->flush();
}

QString post_pair_profile_direct(const archstreamer::PairTarget& target, const QString& profile_json) {
    QTcpSocket socket;
    socket.connectToHost(QString::fromStdString(target.ip), target.port);
    if (!socket.waitForConnected(4000)) {
        return socket.errorString();
    }
    const auto body = profile_json.toUtf8();
    QByteArray request;
    request += "POST /pair HTTP/1.1\r\n";
    request += "Host: " + QByteArray::fromStdString(target.ip) + ":" + QByteArray::number(target.port) + "\r\n";
    request += "Authorization: Bearer " + QByteArray::fromStdString(target.token) + "\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    request += "Connection: close\r\n\r\n";
    request += body;
    socket.write(request);
    if (!socket.waitForBytesWritten(4000) || !socket.waitForReadyRead(6000)) {
        return socket.errorString().isEmpty() ? QStringLiteral("timed out waiting for response") : socket.errorString();
    }
    const auto response = socket.readAll();
    if (!response.startsWith("HTTP/1.1 200") && !response.startsWith("HTTP/1.0 200")) {
        return QString::fromUtf8(response.left(160)).trimmed();
    }
    return {};
}

QString push_pair_profile_relay(
    const archstreamer::PairTarget& target,
    const QString& fallback_host,
    int fallback_port,
    const QString& profile_json) {
    const auto relay_host = !target.relay_host.empty()
        ? target.relay_host
        : fallback_host.trimmed().toStdString();
    const auto relay_port = target.relay_port != 0
        ? target.relay_port
        : static_cast<std::uint16_t>(fallback_port);
    if (relay_host.empty() || relay_port == 0) {
        return QStringLiteral("direct send failed and QR has no relay host");
    }
    try {
        auto stream = archstreamer::TcpStream::connect_to(relay_host, relay_port);
        archstreamer::PairFormRelayPush push;
        push.token = target.token;
        const auto bytes = profile_json.toUtf8();
        push.profile_json.assign(bytes.begin(), bytes.end());
        stream.send_packet(archstreamer::serialize_packet(push));
        const auto packet = stream.receive_packet();
        if (!packet.has_value()) {
            return QStringLiteral("host relay closed without an ack");
        }
        const auto payload = archstreamer::deserialize_packet(*packet);
        if (const auto* ack = std::get_if<archstreamer::PairFormRelayAck>(&payload); ack != nullptr) {
            return ack->ok ? QString() : QString::fromStdString(ack->message);
        }
        if (const auto* error = std::get_if<archstreamer::ErrorPacket>(&payload); error != nullptr) {
            return QString::fromStdString(error->message);
        }
        return QStringLiteral("unexpected relay response");
    } catch (const std::exception& error) {
        return QString::fromUtf8(error.what());
    }
}

std::optional<QString> pull_pair_profile_relay(const QString& host, int port, const QString& token, QString* error) {
    try {
        auto stream = archstreamer::TcpStream::connect_to(host.toStdString(), static_cast<std::uint16_t>(port));
        archstreamer::PairFormRelayPull pull;
        pull.token = token.toStdString();
        stream.send_packet(archstreamer::serialize_packet(pull));
        const auto packet = stream.receive_packet();
        if (!packet.has_value()) {
            if (error != nullptr) *error = QStringLiteral("host relay closed");
            return std::nullopt;
        }
        const auto payload = archstreamer::deserialize_packet(*packet);
        if (const auto* response = std::get_if<archstreamer::PairFormRelayResponse>(&payload); response != nullptr) {
            if (!response->found || response->profile_json.empty()) {
                return std::nullopt;
            }
            return QString::fromUtf8(
                reinterpret_cast<const char*>(response->profile_json.data()),
                static_cast<qsizetype>(response->profile_json.size()));
        }
        if (const auto* err = std::get_if<archstreamer::ErrorPacket>(&payload); err != nullptr) {
            if (error != nullptr) *error = QString::fromStdString(err->message);
        }
    } catch (const std::exception& ex) {
        if (error != nullptr) *error = QString::fromUtf8(ex.what());
    }
    return std::nullopt;
}

} // namespace

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
    if (!client_session_host_.trimmed().isEmpty()) {
        config.host = client_session_host_.trimmed().toStdString();
    } else {
        config.host = client_host_->text().trimmed().toStdString();
    }
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
    config.wanted_bitrate = selected_stream_bitrate();
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

archstreamer::MediaStreamBitrate MainWindow::selected_stream_bitrate() const {
    if (client_stream_bitrate_ == nullptr || client_stream_bitrate_->currentData().isNull()) {
        return archstreamer::MediaStreamBitrate::Auto;
    }
    return static_cast<archstreamer::MediaStreamBitrate>(client_stream_bitrate_->currentData().toInt());
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
    client_session_host_.clear();
    update_client_host_summary(label);
    if (changed) {
        append_log(client_log_, QString("Selected host %1 (control %2, input %3)")
            .arg(address)
            .arg(control_port)
            .arg(input_port));
        refresh_recent_settings_keys();
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

QString MainWindow::current_pair_profile_json() const {
    QJsonObject root;
    root["v"] = 1;
    root["host"] = client_host_ != nullptr ? client_host_->text().trimmed() : QString();
    root["altHost"] = client_alt_host_ != nullptr ? client_alt_host_->text().trimmed() : QString();
    root["controlPort"] = client_port_ != nullptr ? QString::number(client_port_->value()) : QStringLiteral("45555");
    root["inputPort"] = client_input_port_ != nullptr ? QString::number(client_input_port_->value()) : QString::number(DefaultInputPort);
    root["username"] = QString::fromStdString(profile_client_username());
    root["password"] = client_password_ != nullptr ? client_password_->text() : QString();
    root["streamQuality"] = client_stream_quality_ != nullptr ? client_stream_quality_->currentData().toInt() : static_cast<int>(MediaQualityTier::Auto);
    root["streamBitrate"] = client_stream_bitrate_ != nullptr ? client_stream_bitrate_->currentData().toInt() : static_cast<int>(MediaStreamBitrate::Auto);
    root["streamSize"] = client_stream_size_ != nullptr ? client_stream_size_->currentData().toInt() : static_cast<int>(MediaStreamSize::Auto);
    root["streamFeel"] = static_cast<int>(MediaStreamFeel::LowLatency);
    root["remoteSshHost"] = remote_ssh_host_ != nullptr ? remote_ssh_host_->text().trimmed() : QString();
    root["remoteSshUser"] = remote_ssh_user_ != nullptr ? remote_ssh_user_->text().trimmed() : QString();
    root["remoteSshPassword"] = remote_ssh_password_ != nullptr ? remote_ssh_password_->text() : QString();
    root["remoteSshPort"] = remote_ssh_port_ != nullptr ? QString::number(remote_ssh_port_->value()) : QStringLiteral("22");
    root["remoteDirectory"] = remote_directory_ != nullptr ? remote_directory_->text().trimmed() : QString();
    root["remoteRomRoot"] = remote_rom_root_ != nullptr ? remote_rom_root_->text().trimmed() : QString();
    root["remoteBinary"] = remote_binary_ != nullptr ? remote_binary_->text().trimmed() : QStringLiteral("./host_runner");
    root["remoteStartScript"] = remote_start_script_ != nullptr ? remote_start_script_->text().trimmed() : QString();
    root["remoteGpu"] = remote_gpu_ != nullptr ? remote_gpu_->text().trimmed() : QString();
    root["remoteBaseControlPort"] = remote_base_control_port_ != nullptr ? QString::number(remote_base_control_port_->value()) : QString();
    root["remoteBaseInputPort"] = remote_base_input_port_ != nullptr ? QString::number(remote_base_input_port_->value()) : QString();
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void MainWindow::apply_pair_profile_json(const QString& json) {
    const auto document = QJsonDocument::fromJson(json.toUtf8());
    if (!document.isObject()) {
        throw std::runtime_error("pair profile is not a JSON object");
    }
    const auto root = document.object();
    if (client_host_ != nullptr) client_host_->setText(json_string(root, "host"));
    if (client_alt_host_ != nullptr) client_alt_host_->setText(json_string(root, "altHost"));
    if (client_port_ != nullptr) client_port_->setValue(qBound(json_string(root, "controlPort", "45555").toInt(), 1, 65535));
    if (client_input_port_ != nullptr) client_input_port_->setValue(qBound(json_string(root, "inputPort", QString::number(DefaultInputPort)).toInt(), 1, 65535));
    if (profile_username_ != nullptr) profile_username_->setText(json_string(root, "username"));
    if (client_password_ != nullptr) client_password_->setText(json_string(root, "password"));
    set_combo_data(client_stream_quality_, json_int(root, "streamQuality", static_cast<int>(MediaQualityTier::Auto)));
    set_combo_data(client_stream_bitrate_, json_int(root, "streamBitrate", static_cast<int>(MediaStreamBitrate::Auto)));
    set_combo_data(client_stream_size_, json_int(root, "streamSize", static_cast<int>(MediaStreamSize::Auto)));
    if (remote_ssh_host_ != nullptr) remote_ssh_host_->setText(json_string(root, "remoteSshHost"));
    if (remote_ssh_user_ != nullptr) remote_ssh_user_->setText(json_string(root, "remoteSshUser"));
    if (remote_ssh_password_ != nullptr) remote_ssh_password_->setText(json_string(root, "remoteSshPassword"));
    if (remote_ssh_port_ != nullptr) remote_ssh_port_->setValue(qBound(json_string(root, "remoteSshPort", "22").toInt(), 1, 65535));
    if (remote_directory_ != nullptr) remote_directory_->setText(json_string(root, "remoteDirectory"));
    if (remote_rom_root_ != nullptr) remote_rom_root_->setText(json_string(root, "remoteRomRoot"));
    if (remote_binary_ != nullptr) remote_binary_->setText(json_string(root, "remoteBinary", "./host_runner"));
    if (remote_start_script_ != nullptr) remote_start_script_->setText(json_string(root, "remoteStartScript"));
    if (remote_gpu_ != nullptr) remote_gpu_->setText(json_string(root, "remoteGpu"));
    if (remote_base_control_port_ != nullptr) remote_base_control_port_->setValue(qBound(json_string(root, "remoteBaseControlPort", "45555").toInt(), 1, 65535));
    if (remote_base_input_port_ != nullptr) remote_base_input_port_->setValue(qBound(json_string(root, "remoteBaseInputPort", QString::number(DefaultInputPort)).toInt(), 1, 65535));
    client_session_host_.clear();
    update_client_host_summary({});
    refresh_recent_settings_keys();
    save_persisted_settings();
}

void MainWindow::show_pair_receive_qr() {
    close_pair_receive_qr();
    const auto ip = preferred_pair_lan_ip();
    if (ip.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Form sync"), QStringLiteral("No LAN IPv4 address found."));
        return;
    }
    pair_server_ = new QTcpServer(this);
    connect(pair_server_, &QTcpServer::newConnection, this, &MainWindow::handle_pair_receive_socket);
    if (!pair_server_->listen(QHostAddress::AnyIPv4, 0)) {
        const auto error = pair_server_->errorString();
        close_pair_receive_qr(QStringLiteral("Pair receive unavailable: %1").arg(error));
        QMessageBox::warning(this, QStringLiteral("Form sync"), error);
        return;
    }
    pair_token_ = QUuid::createUuid().toString(QUuid::Id128).left(16);
    pair_relay_host_ = client_host_ != nullptr ? client_host_->text().trimmed() : QString();
    pair_relay_port_ = client_port_ != nullptr ? client_port_->value() : 45555;
    const auto uri = QString::fromStdString(archstreamer::build_pair_uri(
        ip.toStdString(),
        static_cast<std::uint16_t>(pair_server_->serverPort()),
        pair_token_.toStdString(),
        pair_relay_host_.toStdString(),
        static_cast<std::uint16_t>(pair_relay_port_)));
    if (client_pair_status_ != nullptr) {
        client_pair_status_->setText(QStringLiteral("Pair receiver listening on %1:%2").arg(ip).arg(pair_server_->serverPort()));
    }

    pair_relay_poll_timer_ = new QTimer(this);
    pair_relay_poll_timer_->setInterval(2000);
    connect(pair_relay_poll_timer_, &QTimer::timeout, this, &MainWindow::poll_pair_relay);
    if (!pair_relay_host_.isEmpty()) {
        pair_relay_poll_timer_->start();
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Receive forms"));
    auto* layout = new QVBoxLayout(&dialog);
    const auto pixmap = render_pair_qr(uri);
    if (!pixmap.isNull()) {
        auto* image = new QLabel(&dialog);
        image->setAlignment(Qt::AlignCenter);
        image->setPixmap(pixmap.scaled(320, 320, Qt::KeepAspectRatio, Qt::FastTransformation));
        layout->addWidget(image);
    } else {
        layout->addWidget(new QLabel(QStringLiteral("qrencode was not found; use the QR text below."), &dialog));
    }
    auto* text = new QPlainTextEdit(uri, &dialog);
    text->setReadOnly(true);
    text->setMaximumHeight(90);
    layout->addWidget(text);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
    if (pair_server_ != nullptr || pair_relay_poll_timer_ != nullptr) {
        close_pair_receive_qr();
    }
}

void MainWindow::close_pair_receive_qr(const QString& status) {
    if (pair_relay_poll_timer_ != nullptr) {
        pair_relay_poll_timer_->stop();
        pair_relay_poll_timer_->deleteLater();
        pair_relay_poll_timer_ = nullptr;
    }
    if (pair_server_ != nullptr) {
        pair_server_->close();
        pair_server_->deleteLater();
        pair_server_ = nullptr;
    }
    pair_token_.clear();
    pair_relay_host_.clear();
    pair_relay_port_ = 0;
    if (client_pair_status_ != nullptr) {
        client_pair_status_->setText(status);
    }
}

void MainWindow::handle_pair_receive_socket() {
    if (pair_server_ == nullptr) {
        return;
    }
    while (auto* socket = pair_server_->nextPendingConnection()) {
        socket->setParent(this);
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
            buffer->append(socket->readAll());
            const auto header_end = buffer->indexOf("\r\n\r\n");
            if (header_end < 0) {
                return;
            }
            const auto headers = buffer->left(header_end);
            const auto match = QRegularExpression(QStringLiteral("(?i)Content-Length:\\s*(\\d+)")).match(QString::fromLatin1(headers));
            const int content_length = match.hasMatch() ? match.captured(1).toInt() : 0;
            if (buffer->size() < header_end + 4 + content_length) {
                return;
            }
            const auto auth = QRegularExpression(QStringLiteral("(?i)Authorization:\\s*Bearer\\s+(\\S+)")).match(QString::fromLatin1(headers));
            if (!auth.hasMatch() || auth.captured(1) != pair_token_) {
                write_http_response(socket, 401, R"({"ok":false,"error":"bad token"})");
            } else if (!headers.startsWith("POST ")) {
                write_http_response(socket, 405, R"({"ok":false,"error":"POST required"})");
            } else {
                const auto body = QString::fromUtf8(buffer->mid(header_end + 4, content_length));
                try {
                    apply_pair_profile_json(body);
                    write_http_response(socket, 200, R"({"ok":true})");
                    close_pair_receive_qr(QStringLiteral("Forms imported from paired device."));
                } catch (const std::exception& error) {
                    write_http_response(socket, 400, R"({"ok":false,"error":"bad profile json"})");
                    if (client_pair_status_ != nullptr) {
                        client_pair_status_->setText(QStringLiteral("Pair receive failed: %1").arg(error.what()));
                    }
                }
            }
            socket->disconnectFromHost();
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }
}

void MainWindow::poll_pair_relay() {
    if (pair_token_.isEmpty() || pair_relay_host_.isEmpty() || pair_relay_port_ <= 0) {
        return;
    }
    if (pair_relay_poll_timer_ != nullptr) {
        pair_relay_poll_timer_->stop();
    }
    const auto token = pair_token_;
    const auto host = pair_relay_host_;
    const int port = pair_relay_port_;
    const QPointer<MainWindow> self(this);
    std::thread([self, token, host, port] {
        QString error;
        const auto profile = pull_pair_profile_relay(host, port, token, &error);
        if (self == nullptr) {
            return;
        }
        QMetaObject::invokeMethod(self.data(), [self, token, profile, error] {
            if (self == nullptr || self->pair_token_ != token) {
                return;
            }
            if (profile.has_value()) {
                try {
                    self->apply_pair_profile_json(*profile);
                    self->close_pair_receive_qr(QStringLiteral("Forms imported through host relay."));
                } catch (const std::exception& ex) {
                    if (self->client_pair_status_ != nullptr) {
                        self->client_pair_status_->setText(QStringLiteral("Pair relay failed: %1").arg(ex.what()));
                    }
                }
                return;
            }
            if (!error.isEmpty() && self->client_pair_status_ != nullptr) {
                self->client_pair_status_->setText(QStringLiteral("Pair relay waiting: %1").arg(error));
            }
            if (self->pair_relay_poll_timer_ != nullptr) {
                self->pair_relay_poll_timer_->start();
            }
        }, Qt::QueuedConnection);
    }).detach();
}

void MainWindow::send_pair_forms_from_qr() {
    bool ok = false;
    const auto raw = QInputDialog::getMultiLineText(
        this,
        QStringLiteral("Send forms from QR"),
        QStringLiteral("Paste the decoded ArchStreamer QR text:"),
        {},
        &ok).trimmed();
    if (!ok || raw.isEmpty()) {
        return;
    }
    std::string error;
    const auto target = archstreamer::parse_pair_uri(raw.toStdString(), &error);
    if (!target.has_value()) {
        QMessageBox::warning(this, QStringLiteral("Form sync"), QString::fromStdString(error));
        return;
    }
    const auto profile = current_pair_profile_json();
    if (client_pair_status_ != nullptr) {
        client_pair_status_->setText(QStringLiteral("Sending forms..."));
    }
    const auto direct_error = post_pair_profile_direct(*target, profile);
    if (direct_error.isEmpty()) {
        if (client_pair_status_ != nullptr) {
            client_pair_status_->setText(QStringLiteral("Forms sent directly to paired device."));
        }
        return;
    }
    append_log(client_log_, QStringLiteral("Pair direct send failed: %1; trying host relay").arg(direct_error));
    const auto relay_error = push_pair_profile_relay(
        *target,
        client_host_ != nullptr ? client_host_->text().trimmed() : QString(),
        client_port_ != nullptr ? client_port_->value() : 45555,
        profile);
    if (client_pair_status_ != nullptr) {
        client_pair_status_->setText(relay_error.isEmpty()
            ? QStringLiteral("Direct send failed; forms handed to host relay.")
            : QStringLiteral("Pair send failed: %1").arg(relay_error));
    }
}

void MainWindow::connect_client() {
    if (client_host_ == nullptr || client_host_->text().trimmed().isEmpty()) {
        append_log(client_log_, "Select a host (Select Host… or This PC) before Connect.");
        return;
    }
    const auto host_text = client_host_->text().trimmed();
    const auto alt_text = client_alt_host_ != nullptr
        ? client_alt_host_->text().trimmed()
        : QString();
    if (!alt_text.isEmpty() && !archstreamer::looks_like_ip_address(alt_text.toStdString())) {
        append_log(
            client_log_,
            "Alt IP must look like an IP address (e.g. 10.6.0.2), or leave it blank.",
            GuiLogLevel::Quiet);
        return;
    }
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
    const auto candidates = archstreamer::host_connect_candidates(
        config.host,
        alt_text.toStdString());
    append_log(client_log_, QString("Connecting to %1:%2...")
        .arg(QString::fromStdString(config.host))
        .arg(config.control_port));
    client_catalog_status_->setText("Connecting...");
    client_connecting_ = true;

    client_connect_thread_ = std::thread([this, config = std::move(config), candidates]() mutable {
        try {
            std::exception_ptr last_error;
            archstreamer::ClientCatalogView catalog;
            std::string used_host = config.host;
            bool connected = false;
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                config.host = candidates[i];
                if (i > 0) {
                    QMetaObject::invokeMethod(
                        this,
                        [this, host = QString::fromStdString(config.host)] {
                            append_log(
                                client_log_,
                                QString("Host unreachable; trying Alt IP %1…").arg(host));
                            client_catalog_status_->setText(
                                QString("Trying Alt IP %1…").arg(host));
                        },
                        Qt::QueuedConnection);
                }
                try {
                    catalog = client_app_.fetch_catalog(config);
                    used_host = config.host;
                    connected = true;
                    break;
                } catch (const std::exception& error) {
                    last_error = std::current_exception();
                    if (i + 1 >= candidates.size() ||
                        !archstreamer::is_tcp_reachability_failure_message(error.what())) {
                        throw;
                    }
                }
            }
            if (!connected) {
                if (last_error) {
                    std::rethrow_exception(last_error);
                }
                throw std::runtime_error("Connect failed");
            }
            QMetaObject::invokeMethod(
                this,
                [this, full = std::move(catalog.full_catalog),
                 art_cache = std::move(catalog.art_cache_root),
                 used_host = QString::fromStdString(used_host)]() mutable {
                    // Keep Host / Alt IP fields as saved prefs; session uses the reachable one.
                    client_session_host_ = used_host;
                    if (client_host_ != nullptr &&
                        client_host_->text().trimmed() != used_host) {
                        append_log(
                            client_log_,
                            QString("Connected via Alt IP %1").arg(used_host));
                    }
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
        const auto password = prompt_session_password("Enter password");
        if (password.isEmpty()) {
            append_log(client_log_, "Password required before joining.");
            return;
        }
        if (client_password_ != nullptr) {
            client_password_->setText(password);
        }
        config.password = password.toStdString();
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
    ds_touch_ = std::make_shared<archstreamer::DsTouchBridge>();
    heartbeat_prefs_ = std::make_shared<archstreamer::ClientHeartbeatPrefs>();
    media_resync_ = std::make_shared<archstreamer::MediaResyncBridge>();
    if (client_resync_av_ != nullptr) {
        client_resync_av_->setEnabled(true);
    }
    {
        std::lock_guard lock(heartbeat_prefs_->mutex);
        heartbeat_prefs_->wanted_tier = config.wanted_tier;
        heartbeat_prefs_->wanted_size = config.wanted_size;
        heartbeat_prefs_->wanted_bitrate = config.wanted_bitrate;
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
        client_video_controller_->setDsTouchBridge(ds_touch_);
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
            callbacks.ds_touch = ds_touch_;
            callbacks.heartbeat_prefs = heartbeat_prefs_;
            callbacks.controller_map_prefs = controller_map_prefs_;
            callbacks.emulator_control = emulator_control_;
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
    ds_touch_.reset();
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
    auto* log = logs_log_ != nullptr
        ? logs_log_
        : (settings_log_ != nullptr ? settings_log_ : client_log_);
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
    auto text = archstreamer::extract_last_log_sessions_from_file(
        gui_log_path(),
        archstreamer::GuiLogSessionMarker,
        sessions);
    if (logs_video_ != nullptr && logs_video_->isChecked()) {
        const auto video_tail = archstreamer::read_log_file_tail(
            archstreamer::gst_video_receiver_log_path());
        if (!video_tail.empty()) {
            text += "\n\n=== gst-video-receiver.log ===\n";
            text += video_tail;
        }
        const auto synced_tail = archstreamer::read_log_file_tail(
            archstreamer::gst_synced_receiver_log_path());
        if (!synced_tail.empty()) {
            text += "\n\n=== gst-synced-media-receiver.log ===\n";
            text += synced_tail;
        }
    }
    if (logs_audio_ != nullptr && logs_audio_->isChecked()) {
        const auto audio_tail = archstreamer::read_log_file_tail(
            archstreamer::gst_audio_receiver_log_path());
        if (!audio_tail.empty()) {
            text += "\n\n=== gst-audio-receiver.log ===\n";
            text += audio_tail;
        }
    }
    if (text.empty()) {
        append_log(log, "Send logs: gui.log is empty or unreadable.", GuiLogLevel::Quiet);
        return;
    }

    archstreamer::ClientLogBundle bundle;
    bundle.username = profile_client_username();
    bundle.session_count = sessions;
    bundle.text.assign(text.begin(), text.end());

    const auto alt = client_alt_host_ != nullptr
        ? client_alt_host_->text().trimmed()
        : QString();
    if (!alt.isEmpty() && !archstreamer::looks_like_ip_address(alt.toStdString())) {
        append_log(log, "Send logs: Alt IP must look like an IP address, or leave it blank.", GuiLogLevel::Quiet);
        return;
    }
    const auto candidates = archstreamer::host_connect_candidates(
        host.toStdString(),
        alt.toStdString());
    if (candidates.empty()) {
        append_log(log, "Send logs: select a host first (Client tab).", GuiLogLevel::Quiet);
        return;
    }

    std::exception_ptr last_error;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        try {
            if (i > 0) {
                append_log(
                    log,
                    QString("Send logs: Host unreachable; trying Alt IP %1…")
                        .arg(QString::fromStdString(candidates[i])));
            }
            auto stream = archstreamer::TcpStream::connect_to(
                candidates[i],
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
            return;
        } catch (const std::exception& error) {
            last_error = std::current_exception();
            if (i + 1 >= candidates.size() ||
                !archstreamer::is_tcp_reachability_failure_message(error.what())) {
                append_log(
                    log,
                    QString("Send logs failed: %1").arg(QString::fromUtf8(error.what())),
                    GuiLogLevel::Quiet);
                return;
            }
        }
    }
    if (last_error) {
        try {
            std::rethrow_exception(last_error);
        } catch (const std::exception& error) {
            append_log(
                log,
                QString("Send logs failed: %1").arg(QString::fromUtf8(error.what())),
                GuiLogLevel::Quiet);
        }
    }
}

QString MainWindow::prompt_session_password(const QString& title) {
    bool ok = false;
    const auto password = QInputDialog::getText(
        this,
        title,
        "Password:",
        QLineEdit::Password,
        {},
        &ok);
    if (!ok || password.isEmpty()) {
        return {};
    }
    return password;
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

    const auto alt = client_alt_host_ != nullptr
        ? client_alt_host_->text().trimmed()
        : QString();
    if (!alt.isEmpty() && !archstreamer::looks_like_ip_address(alt.toStdString())) {
        append_log(log, "Change password: Alt IP must look like an IP address, or leave it blank.", GuiLogLevel::Quiet);
        return;
    }
    const auto primary = !client_session_host_.trimmed().isEmpty()
        ? client_session_host_.trimmed()
        : host;
    const auto candidates = archstreamer::host_connect_candidates(
        primary.toStdString(),
        alt.toStdString());

    std::exception_ptr last_error;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        try {
            if (i > 0) {
                append_log(
                    log,
                    QString("Change password: Host unreachable; trying Alt IP %1…")
                        .arg(QString::fromStdString(candidates[i])));
            }
            auto stream = archstreamer::TcpStream::connect_to(
                candidates[i],
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
            return;
        } catch (const std::exception& error) {
            last_error = std::current_exception();
            if (i + 1 >= candidates.size() ||
                !archstreamer::is_tcp_reachability_failure_message(error.what())) {
                append_log(
                    log,
                    QString("Change password failed: %1").arg(QString::fromUtf8(error.what())),
                    GuiLogLevel::Quiet);
                return;
            }
        }
    }
    if (last_error) {
        try {
            std::rethrow_exception(last_error);
        } catch (const std::exception& error) {
            append_log(
                log,
                QString("Change password failed: %1").arg(QString::fromUtf8(error.what())),
                GuiLogLevel::Quiet);
        }
    }
}

} // namespace archstreamer::gui
