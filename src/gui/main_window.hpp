#pragma once

#include "client/client_app.hpp"
#include "client/game_filter.hpp"
#include "common/discovery.hpp"
#include "common/game_assets.hpp"
#include "common/protocol.hpp"
#include "client_video_controller.hpp"
#include "game_picker_widget.hpp"
#include "gui_logging.hpp"

#ifdef ARCHSTREAMER_HAS_HOST
#include "client/client_media_playback.hpp"
#include "host/media_capture.hpp"
#endif

#include <QMainWindow>
#include <QString>
#include <QStringList>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTimer;
class QWidget;

namespace archstreamer::gui {

class PadOnScreenKeyboard;

class MainWindow final : public QMainWindow {
public:
    MainWindow();
    ~MainWindow() override;

    void apply_debug_profile(const QString& profile);

private:
    QWidget* build_client_tab();
    QWidget* build_remote_tab();
    QWidget* build_stream_tab();
    QWidget* build_game_options_tab();
    QWidget* build_profile_tab();
    QWidget* build_settings_tab();
    void ensure_remote_host();
    void stop_remote_host();
    void set_remote_status(const QString& text);
    void refresh_game_options_ui();
    void sync_controller_map_editor_ui();
    void commit_controller_map_editor_ui();
    ControllerMapFamily selected_controller_map_family() const;
    std::filesystem::path controller_map_file_path() const;
    void load_controller_map_document();
    void save_controller_map_document();

    void refresh_client_controllers();
#ifdef ARCHSTREAMER_HAS_HOST
    void populate_gpu_combo(QComboBox* combo, const QString& previous);
    void refresh_settings_gpus();
    void update_separate_render_gpu_visibility();
    std::string selected_encode_gpu_id() const;
    std::string selected_render_gpu_id() const;
    QString selected_graphics_api_id() const;
    int selected_switch_resolution_scale() const;
    int selected_retroarch_resolution_scale() const;
    QString selected_host_capture_resolution() const;
#endif
    void refresh_settings_audio_outputs(const QString& select_id = {});
    void apply_audio_output_from_settings();
    std::string selected_audio_output_id() const;

#ifdef ARCHSTREAMER_HAS_HOST
    QWidget* build_host_tab();
    void refresh_host_controllers();
    void sync_host_role_and_bridge();
    void sync_host_advertise(bool enabled);
    void advertise_host();
    void load_host_games();
#endif

    void load_persisted_settings();
    void save_persisted_settings();
    void persist_settings_if_idle();
    void remember_session_tab(const QString& tab);
    void restore_last_session_tab();
    void apply_log_level_from_settings();
    GuiLogLevel current_log_level() const;
    int session_timeout_seconds() const;
    std::filesystem::path art_root_path() const;
#ifdef ARCHSTREAMER_HAS_HOST
    std::filesystem::path save_root_path() const;
    void update_save_root_status();
    void browse_save_root();
    void create_save_root();
    void sync_save_root_field_to_path(const std::filesystem::path& path);
    void persist_valid_save_root(const std::filesystem::path& path);
#endif
    std::string steam_account_id_text() const;
    std::string profile_client_username() const;
    std::string profile_host_name() const;
    void apply_art_root_to_pickers();
    void detect_steam_account();
    void refresh_art_from_steam();
    void toggle_pad_on_screen_keyboard(bool open);
    void open_soft_keyboard_from_host(const archstreamer::SoftKeyboardRequest& request);
    void close_pad_on_screen_keyboard();
    void restore_video_window_focus();

    GameFilter client_filter_from_fields() const;
    void refresh_filtered_client_games();
    ClientAppConfig client_config_from_fields() const;
    MediaQualityTier selected_stream_quality() const;
    MediaStreamSize selected_stream_size() const;
    void apply_client_host(
        const QString& address,
        int control_port,
        int input_port,
        const QString& label = {});
    void update_client_host_summary(const QString& label = {});
    void open_host_search_dialog();
    void start_client_host_auto_pick();
    void stop_client_host_auto_pick();
    void connect_client();
    void start_client();
    void stop_client();
    void stop_client_connect();
    void send_client_logs_to_host();
    void change_profile_password_on_host();
    /** Prompt new+confirm; returns empty if cancelled/mismatch. */
    QString prompt_new_password(const QString& title);

#ifdef ARCHSTREAMER_HAS_HOST
    void start_host();
    void stop_host();
    void stop_host_local_media();
    void sync_host_local_media();
#endif

    ClientApp client_app_;
    GameList client_full_catalog_;
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
    std::atomic_bool client_session_live_ = false;
    std::atomic_bool art_refreshing_ = false;
    std::thread client_connect_thread_;
    std::thread client_thread_;
    std::thread art_refresh_thread_;
#ifdef ARCHSTREAMER_HAS_HOST
    QProcess* host_process_ = nullptr;
    QStringList host_debug_args_;
    std::unique_ptr<HostDiscoveryAnnouncer> host_announcer_;
    std::unique_ptr<ClientMediaPlayback> host_local_receiver_;
    std::unique_ptr<ClientVideoController> host_local_video_controller_;
    QTimer* host_local_media_poll_timer_ = nullptr;
    QTimer* host_advertise_timer_ = nullptr;
#endif

    QLineEdit* client_host_ = nullptr;
    QLabel* client_host_summary_ = nullptr;
    /** Session-only join password (Client tab; not persisted). */
    QLineEdit* client_password_ = nullptr;

    QLineEdit* remote_ssh_host_ = nullptr;
    QLineEdit* remote_ssh_user_ = nullptr;
    QLineEdit* remote_ssh_password_ = nullptr;
    QSpinBox* remote_ssh_port_ = nullptr;
    QLineEdit* remote_directory_ = nullptr;
    QLineEdit* remote_rom_root_ = nullptr;
    QLineEdit* remote_binary_ = nullptr;
    QSpinBox* remote_base_control_port_ = nullptr;
    QSpinBox* remote_base_input_port_ = nullptr;
    QLabel* remote_status_ = nullptr;
    QPlainTextEdit* remote_log_ = nullptr;
    bool remote_busy_ = false;
    int remote_tracked_control_port_ = 0;

    QSpinBox* client_port_ = nullptr;
    QSpinBox* client_input_port_ = nullptr;
    QComboBox* client_role_ = nullptr;
    QComboBox* client_mode_ = nullptr;
    QSpinBox* client_players_ = nullptr;
    QCheckBox* client_video_ = nullptr;
    QCheckBox* client_audio_ = nullptr;
    QCheckBox* client_send_keyboard_ = nullptr;
    QComboBox* client_stream_quality_ = nullptr;
    QComboBox* client_stream_size_ = nullptr;
    QCheckBox* client_synced_av_ = nullptr;
    QPushButton* client_resync_av_ = nullptr;
    QLabel* client_catalog_status_ = nullptr;
    GamePickerWidget* client_game_picker_ = nullptr;
    QListWidget* client_controllers_ = nullptr;
    QPlainTextEdit* client_log_ = nullptr;
    std::shared_ptr<DiscControlBridge> disc_control_;
    std::shared_ptr<LinkControlBridge> link_control_;
    std::shared_ptr<SoftKeyboardBridge> soft_keyboard_;
    std::shared_ptr<DsTouchBridge> ds_touch_;
    std::shared_ptr<ClientHeartbeatPrefs> heartbeat_prefs_;
    std::shared_ptr<ClientControllerMapPrefs> controller_map_prefs_;
    std::shared_ptr<EmulatorControlBridge> emulator_control_;
    std::shared_ptr<MediaResyncBridge> media_resync_;
    std::unique_ptr<ClientVideoController> client_video_controller_;
    QComboBox* game_options_map_family_ = nullptr;
    QCheckBox* game_options_swap_nw_ = nullptr;
    QCheckBox* game_options_swap_se_ = nullptr;
    QComboBox* game_options_remap_select_ = nullptr;
    QComboBox* game_options_remap_start_ = nullptr;
    QComboBox* game_options_remap_l_ = nullptr;
    QComboBox* game_options_remap_r_ = nullptr;
    QComboBox* game_options_remap_l2_ = nullptr;
    QComboBox* game_options_remap_r2_ = nullptr;
    QComboBox* game_options_remap_l3_ = nullptr;
    QComboBox* game_options_remap_r3_ = nullptr;
    QPushButton* game_options_pad_osk_ = nullptr;
    QLabel* game_options_status_ = nullptr;
    QComboBox* game_options_disc_ = nullptr;
    QPushButton* game_options_swap_ = nullptr;
    QPushButton* game_options_prev_ = nullptr;
    QPushButton* game_options_next_ = nullptr;
    QLabel* game_options_link_status_ = nullptr;
    QLineEdit* game_options_link_user_ = nullptr;
    QPushButton* game_options_link_request_ = nullptr;
    QPushButton* game_options_link_cancel_ = nullptr;
    QTimer* game_options_poll_timer_ = nullptr;
    std::unique_ptr<HostDiscoveryBrowser> client_auto_browser_;
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
    QComboBox* host_capture_resolution_ = nullptr;
    QCheckBox* host_audio_ = nullptr;
    QCheckBox* host_local_media_ = nullptr;
    QCheckBox* host_advertise_ = nullptr;
    QLabel* host_status_ = nullptr;
    GamePickerWidget* host_game_picker_ = nullptr;
    QPlainTextEdit* host_log_ = nullptr;
#endif

    QLineEdit* profile_username_ = nullptr;
    QLineEdit* profile_host_name_ = nullptr;
    QLineEdit* profile_steam_account_ = nullptr;
    /** Shown only when client_password_ is empty — for Change password. */
    QLineEdit* profile_change_current_password_ = nullptr;
    QWidget* profile_change_current_row_ = nullptr;
    QLineEdit* profile_new_password_ = nullptr;
    QLineEdit* profile_confirm_password_ = nullptr;
    QPushButton* profile_change_password_ = nullptr;
    QPlainTextEdit* profile_log_ = nullptr;

    QLineEdit* settings_art_root_ = nullptr;
    QSpinBox* settings_session_timeout_ = nullptr;
    QComboBox* settings_log_level_ = nullptr;
    QSpinBox* settings_log_sessions_ = nullptr;
    QPushButton* settings_send_logs_ = nullptr;
    QCheckBox* settings_show_framecount_ = nullptr;
#ifdef ARCHSTREAMER_HAS_HOST
    QLineEdit* host_save_root_ = nullptr;
    QLabel* host_save_root_status_ = nullptr;
    QPushButton* host_save_root_browse_ = nullptr;
    QPushButton* host_save_root_create_ = nullptr;
    QCheckBox* settings_allow_new_users_ = nullptr;
    QLineEdit* settings_native_host_runner_ = nullptr;
    QComboBox* settings_gpu_ = nullptr;
    QCheckBox* settings_separate_render_gpu_ = nullptr;
    QComboBox* settings_render_gpu_ = nullptr;
    QComboBox* settings_renderer_ = nullptr;
    QComboBox* settings_switch_scale_ = nullptr;
    QComboBox* settings_retroarch_scale_ = nullptr;
#endif
    QComboBox* settings_audio_out_ = nullptr;
    PadOnScreenKeyboard* pad_osk_ = nullptr;
    std::uint32_t soft_keyboard_request_id_ = 0;
    QPlainTextEdit* settings_log_ = nullptr;
};

} // namespace archstreamer::gui
