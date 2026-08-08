#pragma once

#include "client/client_app.hpp"
#include "client/game_filter.hpp"
#include "common/discovery.hpp"
#include "common/game_assets.hpp"
#include "common/protocol.hpp"
#include "client_video_controller.hpp"
#include "game_picker_widget.hpp"
#include "gui_logging.hpp"
#include "pad_on_screen_keyboard.hpp"
#include "paths_panel.hpp"

#ifdef ARCHSTREAMER_HAS_HOST
#include "client/client_media_playback.hpp"
#include "host/media_capture.hpp"
#include "host/save_manager.hpp"
#endif

#include <QMainWindow>
#include <QString>
#include <QStringList>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>

class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QSettings;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class QTabWidget;
class QTimer;
class QTreeWidget;
class QWidget;

namespace archstreamer::gui {

class MainWindow final : public QMainWindow {
public:
    MainWindow();
    ~MainWindow() override;

    void apply_debug_profile(const QString& profile);
    /** CLI --branch override for this process. Persisted branch still loads first;
     *  this wins when set (see load_persisted_settings). */
    void set_session_update_branch(const QString& branch);

    /**
     * Roots from the Paths tab, `~`-expanded. Empty when the row is blank or the
     * build has no such row, so callers decide what a missing root means.
     * Public because Catalog ops (filesystem renames) resolve them too.
     */
    std::filesystem::path art_root_path() const;
    std::filesystem::path rom_root_path() const;
    std::filesystem::path meta_root_path() const;
    std::filesystem::path save_root_path() const;
    std::filesystem::path dlc_root_path() const;

private:
    QWidget* build_client_tab();
    QWidget* build_remote_tab();
    QWidget* build_stream_tab();
    QWidget* build_controls_tab();
    QWidget* build_game_options_tab();
    QWidget* build_profile_tab();
    QWidget* build_logs_tab();
    QWidget* build_paths_tab();
    QWidget* build_settings_tab();
    void apply_debug_log_flags_from_ui();
    void append_logs_tab(const QString& text, GuiLogLevel level = GuiLogLevel::Normal);
    QWidget* build_updates_group(QWidget* parent);
    void check_for_updates();
    void apply_updates();
    void set_update_status(const QString& text);
    void apply_session_update_branch_to_ui();
    QString update_repo_path() const;
    QString update_branch_name() const;
    QString self_update_script_path(const QString& repo) const;
    void ensure_remote_host();
    void stop_remote_host();
    void refresh_remote_users();
    void kick_remote_user();
    void set_remote_status(const QString& text);
    void refresh_game_options_ui();
    void sync_controller_map_editor_ui();
    void commit_controller_map_editor_ui();
    ControllerMapFamily selected_controller_map_family() const;
    std::filesystem::path controller_map_file_path() const;
    void load_controller_map_document();
    void save_controller_map_document();
    void pull_controls_from_host();
    void push_controls_to_host();
    void set_controls_sync_status(const QString& text);
    void set_controls_sync_busy(bool busy);

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
    QWidget* build_saves_tab();
    QWidget* build_catalog_tab();
    void refresh_host_controllers();
    void sync_host_role_and_bridge();
    void sync_host_advertise(bool enabled);
    void advertise_host();
    void load_host_games();
    void refresh_saves_browser();
    void refresh_saves_system_combo();
    void refresh_saves_browser_list();
    void start_ps2_memcard_prewarm();
    void update_saves_action_enabled();
    bool saves_host_busy() const;
    bool confirm_saves_destructive(const QString& title, const QString& detail);
    void saves_show_context_menu(const QPoint& pos);
    void saves_copy_selection();
    void saves_add_user();
    void saves_remove_user();
    void saves_remove_system();
    void saves_remove_game();
    void saves_kick_user();
    void saves_block_selected_game();
    void saves_unblock_selected_blocked_game();
    void refresh_saves_blocked_list();
    QString saves_selected_username() const;
    std::optional<std::string> saves_selected_catalog_game_id() const;
    SaveNameHints saves_name_hints() const;
    void refresh_catalog_browser();
#endif

    void load_persisted_settings();
    void save_persisted_settings();
    void persist_settings_if_idle();
    void remember_session_tab(const QString& tab);
    void restore_last_session_tab();
    void apply_log_level_from_settings();
    GuiLogLevel current_log_level() const;
    int session_timeout_seconds() const;

    // Paths tab (main_window_paths.cpp). Every one of these tolerates a build
    // whose host-only rows were never created.
    void connect_path_fields();
    void load_path_settings(QSettings& settings);
    void save_path_settings(QSettings& settings);
    /** Flatpak override for the host_runner binary; empty means auto-detect. */
    QString native_host_runner_override() const;
    void update_save_root_status();
    void browse_save_root();
    void create_save_root();
    void sync_save_root_field_to_path(const std::filesystem::path& path);
    void persist_valid_save_root(const std::filesystem::path& path);

    std::string steam_account_id_text() const;
    std::string profile_client_username() const;
    std::string profile_host_name() const;
    /** Recents QSettings key: client uses user+host+port; host uses user. */
    void refresh_recent_settings_keys();
    void apply_art_root_to_pickers();
    void detect_steam_account();
    void refresh_art_from_steam();
    void toggle_pad_on_screen_keyboard(bool open);
    void open_soft_keyboard_from_host(const archstreamer::SoftKeyboardRequest& request);
    void close_pad_on_screen_keyboard();
    /** Drop video out of fullscreen so the pad OSK can stay in front. */
    void prepare_video_for_pad_osk();
    /** Top-level OSK centered on the live video surface (not the main GUI). */
    void place_pad_osk_over_video();
    PadOnScreenKeyboard* make_pad_osk(PadOnScreenKeyboard::Options options);
    void restore_video_window_focus();

    GameFilter client_filter_from_fields() const;
    void refresh_filtered_client_games();
    ClientAppConfig client_config_from_fields() const;
    MediaQualityTier selected_stream_quality() const;
    MediaStreamBitrate selected_stream_bitrate() const;
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
    bool reclaim_matching_host_runners();
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
    // One-shot PS2 memcard parse; the Users tab renders while it runs.
    std::atomic_bool ps2_prewarm_running_ = false;
    bool ps2_prewarm_started_ = false;
    std::thread client_connect_thread_;
    std::thread client_thread_;
    std::thread art_refresh_thread_;
    std::thread ps2_prewarm_thread_;
#ifdef ARCHSTREAMER_HAS_HOST
    QProcess* host_process_ = nullptr;
    QStringList host_debug_args_;
    std::unique_ptr<HostDiscoveryAnnouncer> host_announcer_;
    std::unique_ptr<ClientMediaPlayback> host_local_receiver_;
    QTimer* host_local_media_poll_timer_ = nullptr;
    QTimer* host_advertise_timer_ = nullptr;
#endif

    QLineEdit* client_host_ = nullptr;
    /** Optional backup IP (WireGuard, etc.); tried when Host is unreachable. */
    QLineEdit* client_alt_host_ = nullptr;
    /**
     * Reachable host used after a successful Connect (may be Alt IP).
     * Not persisted — Host / Alt IP fields remain the saved preferences.
     */
    QString client_session_host_;
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
    /** Optional remote wrapper; when set, Ensure Host uses Path B (ports+GPU only). */
    QLineEdit* remote_start_script_ = nullptr;
    QSpinBox* remote_base_control_port_ = nullptr;
    QSpinBox* remote_base_input_port_ = nullptr;
    QLineEdit* remote_gpu_ = nullptr;
    QLabel* remote_status_ = nullptr;
    QListWidget* remote_users_ = nullptr;
    QPushButton* remote_users_refresh_ = nullptr;
    QPushButton* remote_users_kick_ = nullptr;
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
    QComboBox* client_stream_bitrate_ = nullptr;
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
    QLabel* controls_sync_status_ = nullptr;
    QPushButton* controls_sync_pull_ = nullptr;
    QPushButton* controls_sync_push_ = nullptr;
    bool controls_sync_busy_ = false;
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

    /** Every filesystem root the GUI owns; host-only rows stay null client-side. */
    PathsPanel paths_;
    /** Null in a client-only build; art helpers null-check instead of guarding. */
    GamePickerWidget* host_game_picker_ = nullptr;
    std::unique_ptr<ClientVideoController> host_local_video_controller_;

#ifdef ARCHSTREAMER_HAS_HOST
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
    QPlainTextEdit* host_log_ = nullptr;

    QLabel* saves_root_label_ = nullptr;
    QComboBox* saves_user_ = nullptr;
    QComboBox* saves_system_ = nullptr;
    QLineEdit* saves_filter_ = nullptr;
    QTreeWidget* saves_tree_ = nullptr;
    QListWidget* saves_blocked_list_ = nullptr;
    QLabel* saves_blocked_label_ = nullptr;
    QLabel* saves_status_ = nullptr;
    QPushButton* saves_refresh_ = nullptr;
    QAction* saves_add_user_action_ = nullptr;
    QAction* saves_remove_user_action_ = nullptr;
    QAction* saves_remove_system_action_ = nullptr;
    QAction* saves_remove_game_action_ = nullptr;
    QAction* saves_kick_action_ = nullptr;
    QAction* saves_block_game_action_ = nullptr;
    QAction* saves_unblock_game_action_ = nullptr;
    SaveNameHints saves_hints_;
    QTimer* saves_refresh_timer_ = nullptr;
    /** Skip tree rebuilds while the Users context menu is open (shared actions gray out otherwise). */
    bool saves_context_menu_open_ = false;

    QLabel* catalog_db_path_ = nullptr;
    QLabel* catalog_status_ = nullptr;
    QLineEdit* catalog_filter_ = nullptr;
    QPushButton* catalog_refresh_ = nullptr;
    QTableWidget* catalog_meta_table_ = nullptr;
    QTableWidget* catalog_aliases_table_ = nullptr;
    QTableWidget* catalog_user_games_table_ = nullptr;
    QTableWidget* catalog_edits_table_ = nullptr;
    QTableWidget* catalog_play_modes_table_ = nullptr;
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

    QSpinBox* settings_session_timeout_ = nullptr;
    QComboBox* settings_log_level_ = nullptr;
    QSpinBox* settings_log_sessions_ = nullptr;
    QPushButton* settings_send_logs_ = nullptr;
    QCheckBox* logs_controls_ = nullptr;
    QCheckBox* logs_connections_ = nullptr;
    QCheckBox* logs_video_ = nullptr;
    QCheckBox* logs_audio_ = nullptr;
    QPlainTextEdit* logs_log_ = nullptr;
    QCheckBox* settings_show_framecount_ = nullptr;
    QLineEdit* settings_update_repo_ = nullptr;
    QComboBox* settings_update_branch_ = nullptr;
    QLabel* settings_update_status_ = nullptr;
    QPushButton* settings_update_check_ = nullptr;
    QPushButton* settings_update_apply_ = nullptr;
    QString session_update_branch_;
    bool update_busy_ = false;
#ifdef ARCHSTREAMER_HAS_HOST
    QCheckBox* settings_allow_new_users_ = nullptr;
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
