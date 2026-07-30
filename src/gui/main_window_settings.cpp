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

void MainWindow::populate_gpu_combo(QComboBox* combo, const QString& previous) {
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

void MainWindow::refresh_settings_gpus() {
    const auto encode_prev =
        settings_gpu_ != nullptr ? settings_gpu_->currentData().toString() : QStringLiteral("auto");
    const auto render_prev = settings_render_gpu_ != nullptr
        ? settings_render_gpu_->currentData().toString()
        : QStringLiteral("auto");
    populate_gpu_combo(settings_gpu_, encode_prev);
    populate_gpu_combo(settings_render_gpu_, render_prev);
    update_separate_render_gpu_visibility();
}

void MainWindow::update_separate_render_gpu_visibility() {
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

std::string MainWindow::selected_encode_gpu_id() const {
    if (settings_gpu_ == nullptr || settings_gpu_->currentData().isNull()) {
        return "auto";
    }
    const auto id = settings_gpu_->currentData().toString().trimmed();
    return id.isEmpty() ? std::string("auto") : id.toStdString();
}

std::string MainWindow::selected_render_gpu_id() const {
    if (settings_render_gpu_ == nullptr || settings_render_gpu_->currentData().isNull()) {
        return "auto";
    }
    const auto id = settings_render_gpu_->currentData().toString().trimmed();
    return id.isEmpty() ? std::string("auto") : id.toStdString();
}

QString MainWindow::selected_graphics_api_id() const {
    if (settings_renderer_ == nullptr || settings_renderer_->currentData().isNull()) {
        return QStringLiteral("auto");
    }
    const auto id = settings_renderer_->currentData().toString().trimmed();
    return id.isEmpty() ? QStringLiteral("auto") : id;
}

int MainWindow::selected_yuzu_resolution_scale() const {
    if (settings_yuzu_scale_ == nullptr || settings_yuzu_scale_->currentData().isNull()) {
        return 1;
    }
    return qBound(settings_yuzu_scale_->currentData().toInt(), 1, 6);
}

int MainWindow::selected_retroarch_resolution_scale() const {
    if (settings_retroarch_scale_ == nullptr || settings_retroarch_scale_->currentData().isNull()) {
        return 1;
    }
    return qBound(settings_retroarch_scale_->currentData().toInt(), 1, 6);
}

#endif // ARCHSTREAMER_HAS_HOST

void MainWindow::refresh_settings_audio_outputs(const QString& select_id) {
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

void MainWindow::apply_audio_output_from_settings() {
    archstreamer::set_preferred_audio_output_device(selected_audio_output_id());
}

std::string MainWindow::selected_audio_output_id() const {
    if (settings_audio_out_ == nullptr || settings_audio_out_->currentData().isNull()) {
        return "auto";
    }
    const auto id = settings_audio_out_->currentData().toString().trimmed();
    return id.isEmpty() ? std::string("auto") : id.toStdString();
}

void MainWindow::load_persisted_settings() {
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
        host_clients_->setValue(qBound(settings.value("host/maxClients", 2).toInt(), 2, 4));
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

void MainWindow::save_persisted_settings() {
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

void MainWindow::persist_settings_if_idle() {
    if (restoring_settings_) {
        return;
    }
    save_persisted_settings();
}

void MainWindow::remember_session_tab(const QString& tab) {
    if (tab != QStringLiteral("client") && tab != QStringLiteral("host")) {
        return;
    }
    QSettings settings("ArchStreamer", "ArchStreamer");
    settings.setValue("ui/lastSessionTab", tab);
    persist_settings_if_idle();
}

void MainWindow::restore_last_session_tab() {
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

void MainWindow::apply_log_level_from_settings() {
    gui_log_level.store(static_cast<int>(current_log_level()));
}

GuiLogLevel MainWindow::current_log_level() const {
    if (settings_log_level_ == nullptr) {
        return GuiLogLevel::Normal;
    }
    return static_cast<GuiLogLevel>(settings_log_level_->currentData().toInt());
}

int MainWindow::session_timeout_seconds() const {
    if (settings_session_timeout_ == nullptr) {
        return 30;
    }
    return settings_session_timeout_->value();
}

std::filesystem::path MainWindow::art_root_path() const {
    if (settings_art_root_ != nullptr && !settings_art_root_->text().trimmed().isEmpty()) {
        return std::filesystem::path{settings_art_root_->text().trimmed().toStdString()};
    }
    return std::filesystem::path{archstreamer::DefaultArtRoot};
}

std::string MainWindow::steam_account_id_text() const {
    if (profile_steam_account_ == nullptr) {
        return {};
    }
    return profile_steam_account_->text().trimmed().toStdString();
}

std::string MainWindow::profile_client_username() const {
    if (profile_username_ == nullptr) {
        return "local";
    }
    const auto text = profile_username_->text().trimmed();
    return text.isEmpty() ? std::string("local") : text.toStdString();
}

std::string MainWindow::profile_host_name() const {
    if (profile_host_name_ == nullptr) {
        return profile_client_username();
    }
    const auto text = profile_host_name_->text().trimmed();
    return text.isEmpty() ? profile_client_username() : text.toStdString();
}

void MainWindow::apply_art_root_to_pickers() {
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

void MainWindow::detect_steam_account() {
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

void MainWindow::refresh_art_from_steam() {
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

} // namespace archstreamer::gui
