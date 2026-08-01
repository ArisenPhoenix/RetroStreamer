#include "client/client_app.hpp"

#include "client/client_media_playback.hpp"
#include "client/controller_backend.hpp"
#include "client/input_sender.hpp"
#include "client/keyboard_poller.hpp"
#include "client/session_service.hpp"
#include "common/addresses.hpp"
#include "common/link_capability.hpp"
#include "common/platform/default_platform.hpp"
#include "common/serialization.hpp"
#include "common/time.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace archstreamer {
namespace {

bool is_number(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    for (const auto character : value) {
        if (character < '0' || character > '9') {
            return false;
        }
    }
    return true;
}

bool same_controls(const ControllerState& a, const ControllerState& b) {
    return a.buttons == b.buttons &&
        a.left_x == b.left_x &&
        a.left_y == b.left_y &&
        a.right_x == b.right_x &&
        a.right_y == b.right_y &&
        a.left_trigger == b.left_trigger &&
        a.right_trigger == b.right_trigger;
}

bool handle_control_message(TcpStream& stream, const ClientAppCallbacks& callbacks, ClientRunResult& result) {
    if (!stream.readable()) {
        return true;
    }

    const auto payload = receive_client_control_payload(stream);
    if (const auto* ended = std::get_if<SessionEnded>(&payload); ended != nullptr) {
        result.ended_reason = ended->reason;
        if (callbacks.on_session_ended) {
            callbacks.on_session_ended(ended->reason);
        }
        return false;
    }
    if (const auto* error = std::get_if<ErrorPacket>(&payload); error != nullptr) {
        throw std::runtime_error("host ended session: " + error->message);
    }
    if (const auto* disc = std::get_if<DiscControlResponse>(&payload); disc != nullptr) {
        if (callbacks.disc_control) {
            callbacks.disc_control->set_response(*disc);
        }
        if (callbacks.on_status) {
            callbacks.on_status(
                disc->ok ? ("Disc control: " + disc->message)
                         : ("Disc control failed: " + disc->message));
        }
    }
    if (const auto* link = std::get_if<LinkResponse>(&payload); link != nullptr) {
        if (callbacks.link_control) {
            callbacks.link_control->set_response(*link);
        }
        if (callbacks.on_status) {
            callbacks.on_status(
                link->ok ? ("Link: " + link->message)
                         : ("Link failed: " + link->message));
        }
    }
    if (const auto* soft_keyboard = std::get_if<SoftKeyboardRequest>(&payload);
        soft_keyboard != nullptr) {
        if (callbacks.soft_keyboard) {
            callbacks.soft_keyboard->set_request(*soft_keyboard);
        }
        if (callbacks.on_status) {
            callbacks.on_status(
                "Host requested pad keyboard" +
                (soft_keyboard->prompt.empty()
                     ? std::string{}
                     : (": " + soft_keyboard->prompt)));
        }
    }
    // Mid-session MediaEndpoint after a quality cutover updates the video URI only.
    // Do not realign audio — Opus is still on the live timeline.
    if (const auto* endpoint = std::get_if<MediaEndpoint>(&payload); endpoint != nullptr) {
        result.media_endpoint = *endpoint;
        if (callbacks.on_status) {
            callbacks.on_status("Host media endpoint updated.");
        }
    }
    if (const auto* pending = std::get_if<MediaVideoPending>(&payload); pending != nullptr) {
        if (callbacks.video_cutover) {
            callbacks.video_cutover->set_pending(pending->video_uri);
        }
        if (callbacks.on_status) {
            callbacks.on_status("Host staging new video quality…");
        }
    }

    return true;
}

std::vector<std::string> selected_device_ids(
    const std::vector<ControllerDevice>& devices,
    const ClientAppConfig& config) {
    std::vector<std::string> ids;
    ids.reserve(config.filter.requested_players);
    for (LocalPlayerIndex player = 0; player < config.filter.requested_players; ++player) {
        const auto device_index = config.controller_indexes[player];
        if (device_index >= devices.size()) {
            throw std::runtime_error("controller index is out of range");
        }
        ids.push_back(devices[device_index].id);
    }
    return ids;
}

std::vector<ControllerInfo> controller_info_for_selection(
    const std::vector<ControllerDevice>& devices,
    const ClientAppConfig& config) {
    std::vector<ControllerInfo> controllers;
    controllers.reserve(config.filter.requested_players);
    for (LocalPlayerIndex player = 0; player < config.filter.requested_players; ++player) {
        const auto device_index = config.controller_indexes[player];
        if (device_index >= devices.size()) {
            throw std::runtime_error("controller index is out of range");
        }

        controllers.push_back(ControllerInfo{
            player,
            devices[device_index].name,
            devices[device_index].guid,
            devices[device_index].vendor_id,
            devices[device_index].product_id,
        });
    }
    return controllers;
}

} // namespace

std::optional<GameId> select_game_id(const GameList& list, const std::optional<std::string>& selector) {
    if (!selector.has_value()) {
        if (list.games.empty()) {
            return std::nullopt;
        }
        return list.games.front().id;
    }
    if (selector->empty()) {
        return std::nullopt;
    }
    if (is_number(*selector)) {
        const auto index = static_cast<std::size_t>(std::stoul(*selector));
        if (index >= list.games.size()) {
            throw std::runtime_error("game index is out of range");
        }
        return list.games[index].id;
    }

    for (const auto& game : list.games) {
        if (game.id == *selector) {
            return game.id;
        }
    }
    throw std::runtime_error("game id was not found in the host game list");
}

bool contains_game_id(const GameList& list, const GameId& game_id) {
    for (const auto& game : list.games) {
        if (game.id == game_id) {
            return true;
        }
    }

    return false;
}

std::vector<ControllerDevice> ClientApp::list_controllers() const {
    ControllerBackend backend;
    return backend.list_devices();
}

ActiveSessionInfo ClientApp::active_session_info(const std::string& host, std::uint16_t control_port) const {
    return ClientSessionService(host, control_port).active_session_info();
}

ClientCatalogView ClientApp::fetch_catalog(
    const ClientAppConfig& config,
    const ClientAppCallbacks& callbacks) const {
    ClientSessionService session_service(config.host, config.control_port);
    auto pending_session = session_service.begin();
    auto filtered_catalog = filter_games(pending_session.game_list, config.filter);
    if (callbacks.on_catalog) {
        callbacks.on_catalog(pending_session.game_list, filtered_catalog);
    }

    return ClientCatalogView{
        std::move(pending_session.game_list),
        std::move(filtered_catalog),
        std::move(pending_session.art_cache_root),
    };
}

ClientSessionDraft ClientApp::begin_session(
    const ClientAppConfig& config,
    const ClientAppCallbacks& callbacks) const {
    ClientSessionService session_service(config.host, config.control_port);
    auto pending_session = session_service.begin();
    auto filtered_catalog = filter_games(pending_session.game_list, config.filter);
    if (callbacks.on_catalog) {
        callbacks.on_catalog(pending_session.game_list, filtered_catalog);
    }

    return ClientSessionDraft{
        std::move(pending_session),
        std::move(filtered_catalog),
    };
}

ClientRunResult ClientApp::join_session(
    ClientSessionDraft draft,
    const ClientAppConfig& config,
    const std::function<bool()>& should_stop,
    const ClientAppCallbacks& callbacks) const {
    if (!valid_username(config.username)) {
        throw std::runtime_error("username must be 1-64 characters and contain only letters, numbers, underscores, or hyphens");
    }
    if (!valid_player_count(config.filter.requested_players)) {
        throw std::runtime_error("requested players must be 0, 1, or 2");
    }
    if (config.role == ClientParticipantRole::Viewer && config.filter.requested_players != 0) {
        throw std::runtime_error("viewer clients cannot request player seats");
    }
    if (config.controller_indexes.size() < config.filter.requested_players) {
        throw std::runtime_error("not enough selected controllers for requested players");
    }
    if (config.controller_indexes.size() > MaxPlayersPerClient) {
        throw std::runtime_error("client can select at most two controllers");
    }

    ClientRunResult result;
    result.full_catalog = draft.pending_session.game_list;
    result.filtered_catalog = draft.filtered_catalog;

    result.selected_game_id = select_game_id(result.filtered_catalog, config.game_selector);
    if (result.selected_game_id.has_value() && !contains_game_id(result.full_catalog, *result.selected_game_id)) {
        throw std::runtime_error("selected game is not in the host game list");
    }
    if (!result.selected_game_id.has_value() && result.filtered_catalog.games.empty()) {
        throw std::runtime_error("no games match the selected filters");
    }

    auto controller_device_ids = std::vector<std::string>{};
    auto controllers = std::vector<ControllerInfo>{};
    if (config.filter.requested_players > 0) {
        ControllerBackend backend;
        const auto devices = backend.list_devices();
        controller_device_ids = selected_device_ids(devices, config);
        controllers = controller_info_for_selection(devices, config);
    }

    const auto hello = draft.pending_session.session.make_hello(
        config.username,
        config.display_name.empty() ? config.username : config.display_name,
        result.selected_game_id,
        config.session_mode,
        config.filter.requested_players,
        std::move(controllers),
        config.wants_video,
        config.wants_audio);
    ClientSessionService session_service(config.host, config.control_port);
    auto joined_session = session_service.finish_join(std::move(draft.pending_session), hello);
    auto& session = joined_session.session;

    result.client_id = session.client_id();
    result.seats = session.seats();
    result.ready = joined_session.ready;
    if (callbacks.on_connected && result.client_id.has_value()) {
        callbacks.on_connected(ClientConnectionInfo{
            *result.client_id,
            config.username,
            config.role,
            config.session_mode,
            result.selected_game_id,
        });
    }
    if (callbacks.on_seat_assignment) {
        callbacks.on_seat_assignment(result.seats);
    }
    if (callbacks.on_session_ready) {
        callbacks.on_session_ready(result.ready);
    }

    const auto start = session_service.wait_for_starting(joined_session.stream);
    result.starting = start.starting;
    result.media_endpoint = start.media_endpoint;

    auto media_receiver = ClientMediaPlayback{};
    auto video_cutover = callbacks.video_cutover;
    if (!video_cutover) {
        video_cutover = std::make_shared<MediaVideoCutoverBridge>();
    }
    // Ensure control-message handling can queue staging URIs for this session.
    ClientAppCallbacks session_callbacks = callbacks;
    session_callbacks.video_cutover = video_cutover;
    const bool expect_video =
        config.wants_video &&
        result.media_endpoint.has_value() &&
        !result.media_endpoint->video_uri.empty();
    const bool expect_audio =
        config.wants_audio &&
        result.media_endpoint.has_value() &&
        !result.media_endpoint->audio_uri.empty();
    if (expect_video || expect_audio) {
        if (callbacks.on_media_endpoint) {
            callbacks.on_media_endpoint(*result.media_endpoint);
        }
        const auto endpoint = MediaEndpoint{
            expect_video ? result.media_endpoint->video_uri : "",
            expect_audio ? result.media_endpoint->audio_uri : "",
        };
        const auto strategy = config.synced_av
            ? ClientMediaPlayback::Strategy::Synced
            : ClientMediaPlayback::Strategy::Legacy;
        media_receiver.connect(endpoint, strategy);
        if (config.synced_av && callbacks.on_status) {
            callbacks.on_status("Using synced A/V pipeline (shared GStreamer clock).");
        }
        if (callbacks.on_status) {
            if (!media_receiver.video_pipeline_info().empty()) {
                callbacks.on_status("Video pipeline: " + media_receiver.video_pipeline_info());
            }
            if (!media_receiver.audio_pipeline_info().empty()) {
                callbacks.on_status("Audio pipeline: " + media_receiver.audio_pipeline_info());
            }
            if (expect_video) {
                const auto port = video_port_from_endpoint(*result.media_endpoint);
                callbacks.on_status(
                    "If no video window appears, open UDP " + std::to_string(port) +
                    "+ on THIS client (firewalld/ufw). Control/input can work while media is blocked.");
            }
        }
    }
    if (callbacks.on_session_starting) {
        callbacks.on_session_starting(result.starting);
    }
    if (callbacks.disc_control) {
        std::lock_guard lock(callbacks.disc_control->mutex);
        callbacks.disc_control->session_active = true;
        callbacks.disc_control->active_game_id = result.selected_game_id.value_or(GameId{});
        callbacks.disc_control->disc_labels.clear();
        for (const auto& game : result.full_catalog.games) {
            if (result.selected_game_id.has_value() && game.id == *result.selected_game_id) {
                callbacks.disc_control->disc_labels = game.playlist_discs;
                break;
            }
        }
    }
    if (callbacks.link_control) {
        std::lock_guard lock(callbacks.link_control->mutex);
        callbacks.link_control->session_active = true;
        callbacks.link_control->active_game_id = result.selected_game_id.value_or(GameId{});
        callbacks.link_control->system_key.clear();
        callbacks.link_control->link_capable = false;
        for (const auto& game : result.full_catalog.games) {
            if (result.selected_game_id.has_value() && game.id == *result.selected_game_id) {
                callbacks.link_control->system_key = game.system_key;
                callbacks.link_control->link_capable = system_supports_link(game.system_key);
                break;
            }
        }
    }

    auto controller_backend = std::optional<ControllerBackend>{};
    auto input_sender = std::optional<InputSender>{};
    auto input_socket = std::optional<UdpSocket>{};
    std::atomic<bool> input_stop{false};
    std::thread input_thread;
    const bool want_pads =
        config.input_port.has_value() &&
        config.filter.requested_players > 0 &&
        result.client_id.has_value();
    // Viewers request 0 pads but still need remoted keyboard (Space=FF, P=pause).
    const bool want_keyboard =
        config.input_port.has_value() &&
        config.send_keyboard &&
        result.client_id.has_value();
    if (want_pads || want_keyboard) {
        if (want_pads) {
            controller_backend.emplace();
            controller_backend->open_selected(controller_device_ids);
        }
        input_sender.emplace(*result.client_id);
        input_socket.emplace();
        // Build the poller on the session worker thread so status lands in the GUI log
        // (the input thread must not touch Qt widgets).
        std::unique_ptr<KeyboardPoller> keyboard_poller;
        if (config.send_keyboard) {
            keyboard_poller = std::make_unique<KeyboardPoller>();
            if (callbacks.on_status) {
                callbacks.on_status(keyboard_poller->backend_status());
            }
        }
        if (want_pads && callbacks.on_input_streaming_started) {
            callbacks.on_input_streaming_started(config.host, *config.input_port);
        } else if (want_keyboard && callbacks.on_status) {
            callbacks.on_status(
                "Sending remoted keyboard to " + config.host + ":" +
                std::to_string(*config.input_port));
        }

        // Dedicated thread: media/TCP work on the session loop must not stall pads.
        // ~250 Hz keepalive + triple-send on edges (fresh timestamps) for Wi‑Fi loss.
        input_thread = std::thread([
            &input_stop,
            &controller_backend,
            &input_sender,
            &input_socket,
            &config,
            want_pads,
            keyboard_poller = std::move(keyboard_poller)
        ]() mutable {
            std::array<ControllerState, MaxPlayersPerClient> last_sent{};
            std::array<bool, MaxPlayersPerClient> have_last_sent{};
            KeyboardState last_keys{};
            bool have_last_keys = false;
            constexpr auto kInputTick = std::chrono::milliseconds(4);
            constexpr int kChangeCopies = 3;
            while (!input_stop.load(std::memory_order_relaxed)) {
                const auto tick_start = std::chrono::steady_clock::now();
                if (want_pads && controller_backend.has_value()) {
                    for (LocalPlayerIndex player = 0; player < config.filter.requested_players; ++player) {
                        const auto state = controller_backend->poll(player);
                        if (!state.has_value()) {
                            continue;
                        }
                        const bool changed =
                            !have_last_sent[player] || !same_controls(last_sent[player], *state);
                        // Always send each tick so lost button-down edges recover quickly.
                        const int copies = changed ? kChangeCopies : 1;
                        for (int copy = 0; copy < copies; ++copy) {
                            auto sample = *state;
                            // Distinct timestamps so host ordering accepts each UDP copy.
                            sample.timestamp_us =
                                archstreamer::steady_timestamp_us() + static_cast<std::uint64_t>(copy);
                            if (copy > 0) {
                                sample.sequence = state->sequence + static_cast<std::uint32_t>(copy);
                            }
                            const auto packet = input_sender->make_input(player, sample);
                            try {
                                input_socket->send_to(
                                    serialize_packet(packet),
                                    config.host,
                                    *config.input_port);
                            } catch (...) {
                                // Transient send failures should not kill the session loop.
                            }
                        }
                        last_sent[player] = *state;
                        have_last_sent[player] = true;
                    }
                }

                if (config.send_keyboard && keyboard_poller) {
                    if (const auto keys = keyboard_poller->poll(); keys.has_value()) {
                        const bool changed = !have_last_keys || !same_keys(last_keys, *keys);
                        const int copies = changed ? kChangeCopies : 1;
                        for (int copy = 0; copy < copies; ++copy) {
                            auto sample = *keys;
                            sample.timestamp_us =
                                archstreamer::steady_timestamp_us() + static_cast<std::uint64_t>(copy);
                            if (copy > 0) {
                                sample.sequence = keys->sequence + static_cast<std::uint32_t>(copy);
                            }
                            const auto packet = input_sender->make_keyboard(0, sample);
                            try {
                                input_socket->send_to(
                                    serialize_packet(packet),
                                    config.host,
                                    *config.input_port);
                            } catch (...) {
                            }
                        }
                        last_keys = *keys;
                        have_last_keys = true;
                    }
                }

                const auto elapsed = std::chrono::steady_clock::now() - tick_start;
                if (elapsed < kInputTick) {
                    std::this_thread::sleep_for(kInputTick - elapsed);
                }
            }
        });
    } else if (callbacks.on_waiting_without_input) {
        callbacks.on_waiting_without_input();
    }

    std::uint32_t heartbeat_sequence = 0;
    std::uint64_t last_decoded_frames = 0;
    bool video_was_running = false;
    auto next_heartbeat = std::chrono::steady_clock::now();
    auto media_watch_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto media_watch_armed = static_cast<bool>(media_receiver);
    // Cooldown so host ladder hints / rapid Resync clicks do not restart in a loop.
    auto next_resync_allowed = std::chrono::steady_clock::now();
    int zero_frame_streak = 0;
    bool audio_realign_after_video_stall = false;
    // Retry guard: a failed swap must not respin every loop iteration.
    std::string attempted_video_switch;
    while (!should_stop()) {
        if (!handle_control_message(joined_session.stream, session_callbacks, result)) {
            break;
        }
        if (const auto pending = video_cutover->take_pending(); pending.has_value()) {
            if (media_receiver.begin_video_pending(*pending)) {
                if (session_callbacks.on_status) {
                    session_callbacks.on_status("Receiving staged video quality…");
                }
            } else if (session_callbacks.on_status) {
                session_callbacks.on_status("Failed to open staged video path.");
            }
        }
        if (const auto ready_uri = media_receiver.poll_video_cutover(); ready_uri.has_value()) {
            try {
                joined_session.stream.send_packet(serialize_packet(MediaVideoReady{*ready_uri}));
                if (session_callbacks.on_status) {
                    session_callbacks.on_status("Staged video verified; waiting for host to swap.");
                }
            } catch (const std::exception& error) {
                if (session_callbacks.on_status) {
                    session_callbacks.on_status(
                        std::string("Failed to ACK video cutover: ") + error.what());
                }
            }
        }
        // The host publishes the promoted endpoint once it has torn the old
        // encode down; moving before that would point us at a dead port.
        if (result.media_endpoint.has_value() && media_receiver &&
            !result.media_endpoint->video_uri.empty() &&
            result.media_endpoint->video_uri != media_receiver.endpoint().video_uri &&
            result.media_endpoint->video_uri != attempted_video_switch) {
            attempted_video_switch = result.media_endpoint->video_uri;
            if (media_receiver.switch_video(attempted_video_switch)) {
                if (session_callbacks.on_status) {
                    session_callbacks.on_status("Switched to new video quality.");
                }
            } else if (session_callbacks.on_status) {
                session_callbacks.on_status("Failed to switch to new video quality.");
            }
        }
        if (callbacks.disc_control) {
            if (const auto request = callbacks.disc_control->take_pending(); request.has_value()) {
                try {
                    joined_session.stream.send_packet(serialize_packet(*request));
                } catch (const std::exception& error) {
                    if (callbacks.on_status) {
                        callbacks.on_status(std::string("Failed to send disc request: ") + error.what());
                    }
                }
            }
        }
        if (callbacks.link_control) {
            if (const auto request = callbacks.link_control->take_pending(); request.has_value()) {
                try {
                    joined_session.stream.send_packet(serialize_packet(*request));
                } catch (const std::exception& error) {
                    if (callbacks.on_status) {
                        callbacks.on_status(std::string("Failed to send link request: ") + error.what());
                    }
                }
            }
        }
        if (callbacks.soft_keyboard) {
            if (const auto response = callbacks.soft_keyboard->take_response(); response.has_value()) {
                try {
                    joined_session.stream.send_packet(serialize_packet(*response));
                } catch (const std::exception& error) {
                    if (callbacks.on_status) {
                        callbacks.on_status(
                            std::string("Failed to send soft keyboard response: ") + error.what());
                    }
                }
            }
        }
        if (joined_session.stream.peer_closed()) {
            result.host_disconnected = true;
            if (callbacks.on_host_disconnected) {
                callbacks.on_host_disconnected();
            }
            break;
        }

        if (expect_video && media_receiver) {
            if (media_receiver.video_running()) {
                video_was_running = true;
            } else if (video_was_running) {
                // Closing the sink window (or a mid-session pipeline crash) stops gst-launch.
                // Tear media down immediately so a leftover audio process cannot keep playing.
                media_receiver.disconnect();
                result.ended_reason = "video window closed";
                if (callbacks.on_session_ended) {
                    callbacks.on_session_ended(*result.ended_reason);
                }
                break;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (media_receiver) {
            if (media_receiver.poll() && callbacks.on_status) {
                callbacks.on_status("Audio output rebound.");
            }
        }
        if (controller_backend.has_value()) {
            if (auto hotplug = controller_backend->take_hotplug_status();
                hotplug.has_value() && callbacks.on_status) {
                callbacks.on_status(*hotplug);
            }
        }
        const bool want_resync =
            callbacks.media_resync && callbacks.media_resync->take();
        if (want_resync && media_receiver.has_endpoint() && now >= next_resync_allowed) {
            // Legacy: restart audio only so it meets the current video edge.
            // Synced: full pipeline restart (single process).
            if (media_receiver.resync_audio()) {
                next_resync_allowed = now + std::chrono::seconds(15);
                zero_frame_streak = 0;
                audio_realign_after_video_stall = false;
                last_decoded_frames = media_receiver.decoded_frame_count();
                if (callbacks.on_status) {
                    callbacks.on_status("Realigned audio to video.");
                }
            }
        }
        if (media_watch_armed && now >= media_watch_deadline) {
            media_watch_armed = false;
            if (expect_video && media_receiver && !media_receiver.video_running()) {
                if (callbacks.on_status) {
                    callbacks.on_status("Video receiver died — check GStreamer plugins / display sink.");
                }
            } else if (expect_audio && media_receiver && !media_receiver.audio_running()) {
                if (callbacks.on_status) {
                    callbacks.on_status("Audio receiver died — check GStreamer Opus/Pulse plugins.");
                }
            } else if (expect_video && media_receiver && !media_receiver.video_frames_seen()) {
                if (callbacks.on_status) {
                    callbacks.on_status(
                        "Video receiver is up but no decoded frames yet. "
                        "Usually RTP video datagrams are dropped on Wi‑Fi/VPN (host now uses mtu=1200), "
                        "or the host capture display has no picture. See gst-video-receiver.log in cache.");
                }
            } else if (callbacks.on_status) {
                callbacks.on_status("Media receivers still running; decoded video frames are flowing.");
            }
        }
        if (now >= next_heartbeat && result.client_id.has_value()) {
            std::uint16_t frames_delta = 0;
            std::uint16_t loss_permille = 0;
            if (expect_video && media_receiver) {
                const auto frames = media_receiver.decoded_frame_count();
                const auto delta = frames >= last_decoded_frames ? frames - last_decoded_frames : 0;
                last_decoded_frames = frames;
                frames_delta = static_cast<std::uint16_t>(std::min<std::uint64_t>(delta, 65535));
                // Only invent loss when the receiver is actually dead. A zero-frame
                // second is reported via frames_decoded_delta for host hysteresis;
                // mapping it to 500‰ made Auto treat every decode hiccup as 50% loss.
                if (!media_receiver.video_running()) {
                    loss_permille = 1000;
                }

                // Video stall while audio keeps free-running → audio gets ahead.
                // When frames return, restart audio only so it meets the live video edge.
                if (media_receiver.video_running() && media_receiver.audio_running() &&
                    media_receiver.has_endpoint()) {
                    if (frames_delta == 0) {
                        if (last_decoded_frames > 0) {
                            ++zero_frame_streak;
                            if (zero_frame_streak >= 3) {
                                audio_realign_after_video_stall = true;
                            }
                        }
                    } else {
                        if (audio_realign_after_video_stall && now >= next_resync_allowed) {
                            if (media_receiver.resync_audio()) {
                                next_resync_allowed = now + std::chrono::seconds(15);
                                last_decoded_frames = media_receiver.decoded_frame_count();
                                if (callbacks.on_status) {
                                    callbacks.on_status(
                                        "Video recovered; restarted audio to match (lip-sync).");
                                }
                            }
                        }
                        zero_frame_streak = 0;
                        audio_realign_after_video_stall = false;
                    }
                }
            }
            auto wanted_tier = config.wanted_tier;
            auto max_bitrate_kbps = config.max_bitrate_kbps;
            auto show_framecount = config.show_framecount;
            if (callbacks.heartbeat_prefs) {
                callbacks.heartbeat_prefs->snapshot(wanted_tier, max_bitrate_kbps, show_framecount);
            }
            joined_session.stream.send_packet(serialize_packet(ViewerHeartbeat{
                *result.client_id,
                heartbeat_sequence++,
                loss_permille,
                frames_delta,
                wanted_tier,
                max_bitrate_kbps,
                show_framecount,
            }));
            next_heartbeat = now + std::chrono::seconds(1);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Tell the host this was intentional (Stop / video close). Unexpected TCP drops
    // omit this packet so the host keeps seats for the reconnect window.
    if (!result.host_disconnected && result.client_id.has_value()) {
        const bool host_ended_session =
            result.ended_reason.has_value() &&
            *result.ended_reason != "video window closed";
        if (!host_ended_session) {
            const auto reason = result.ended_reason.value_or("client stopped");
            try {
                joined_session.stream.send_packet(serialize_packet(ClientSessionLeave{
                    *result.client_id,
                    reason,
                }));
            } catch (const std::exception& error) {
                if (callbacks.on_status) {
                    callbacks.on_status(
                        std::string("Failed to send session leave: ") + error.what());
                }
            }
        }
    }

    input_stop.store(true, std::memory_order_relaxed);
    if (input_thread.joinable()) {
        input_thread.join();
    }
    if (media_receiver) {
        media_receiver.disconnect();
    }
    if (callbacks.disc_control) {
        std::lock_guard lock(callbacks.disc_control->mutex);
        callbacks.disc_control->session_active = false;
    }
    if (callbacks.link_control) {
        std::lock_guard lock(callbacks.link_control->mutex);
        callbacks.link_control->session_active = false;
        callbacks.link_control->link_capable = false;
    }

    return result;
}

ClientRunResult ClientApp::run_session(
    const ClientAppConfig& config,
    const std::function<bool()>& should_stop,
    const ClientAppCallbacks& callbacks) const {
    auto draft = begin_session(config, callbacks);
    return join_session(std::move(draft), config, should_stop, callbacks);
}

} // namespace archstreamer
